#include "spec.h"
#include "field_spec.h"
#include "document.h"
#include <assert.h>
#include "util/dict.h"
#include "query_node.h"
#include "search_options.h"
#include "query_internal.h"
#include "numeric_filter.h"
#include "query.h"
#include "indexer.h"
#include "extension.h"
#include "ext/default.h"
#include <float.h>
#include "rwlock.h"
#include "fork_gc.h"

int RediSearch_GetCApiVersion() {
  return REDISEARCH_CAPI_VERSION;
}

static dictType invidxDictType = {0};

static void valFreeCb(void* unused, void* p) {
  KeysDictValue* kdv = p;
  if (kdv->dtor) {
    kdv->dtor(kdv->p);
  }
  free(kdv);
}

IndexSpec* RediSearch_CreateIndex(const char* name, const RSIndexOptions* options) {
  RSIndexOptions opts_s = {.gcPolicy = GC_POLICY_FORK};
  if (!options) {
    options = &opts_s;
  }
  IndexSpec* spec = NewIndexSpec(name);
  spec->flags |= Index_Temporary;  // temporary is so that we will not use threads!!
  if (!spec->indexer) {
    spec->indexer = NewIndexer(spec);
  }

  // Initialize only once:
  if (!invidxDictType.valDestructor) {
    invidxDictType = dictTypeHeapRedisStrings;
    invidxDictType.valDestructor = valFreeCb;
  }
  spec->getValue = options->gvcb;
  spec->getValueCtx = options->gvcbData;
  spec->keysDict = dictCreate(&invidxDictType, NULL);
  spec->minPrefix = 0;
  spec->maxPrefixExpansions = -1;
  if (options->flags & RSIDXOPT_DOCTBLSIZE_UNLIMITED) {
    spec->docs.maxSize = DOCID_MAX;
  }
  if (options->gcPolicy != GC_POLICY_NONE) {
    IndexSpec_StartGCFromSpec(spec, GC_DEFAULT_HZ, options->gcPolicy);
  }
  return spec;
}

void RediSearch_DropIndex(IndexSpec* sp) {
  RWLOCK_ACQUIRE_WRITE();

  if (sp->gc) {
    // for now this is good enough, we should add another api to GC called before its freed.
    ((ForkGCCtx*)(sp->gc->gcCtx))->type = ForkGCCtxType_FREED;
  }
  dict* d = sp->keysDict;
  dictRelease(d);
  sp->keysDict = NULL;
  IndexSpec_FreeSync(sp);

  RWLOCK_RELEASE();
}

RSField* RediSearch_CreateField(IndexSpec* sp, const char* name, unsigned types, unsigned options) {
  assert(types);
  RWLOCK_ACQUIRE_WRITE();

  RSField* fs = IndexSpec_CreateField(sp, name);
  int numTypes = 0;

  if (types & RSFLDTYPE_FULLTEXT) {
    numTypes++;
    int txtId = IndexSpec_CreateTextId(sp);
    if (txtId < 0) {
      RWLOCK_RELEASE();
      return NULL;
    }
    fs->ftId = txtId;
    FieldSpec_Initialize(fs, INDEXFLD_T_FULLTEXT);
  }

  if (types & RSFLDTYPE_NUMERIC) {
    numTypes++;
    FieldSpec_Initialize(fs, INDEXFLD_T_NUMERIC);
  }
  if (types & RSFLDTYPE_GEO) {
    FieldSpec_Initialize(fs, INDEXFLD_T_GEO);
    numTypes++;
  }
  if (types & RSFLDTYPE_TAG) {
    FieldSpec_Initialize(fs, INDEXFLD_T_TAG);
    numTypes++;
  }

  if (numTypes > 1) {
    fs->options |= FieldSpec_Dynamic;
  }

  if (options & RSFLDOPT_NOINDEX) {
    fs->options |= FieldSpec_NotIndexable;
  }
  if (options & RSFLDOPT_SORTABLE) {
    fs->options |= FieldSpec_Sortable;
    fs->sortIdx = RSSortingTable_Add(sp->sortables, fs->name, fieldTypeToValueType(fs->types));
  }
  if (options & RSFLDOPT_TXTNOSTEM) {
    fs->options |= FieldSpec_NoStemming;
  }
  if (options & RSFLDOPT_TXTPHONETIC) {
    fs->options |= FieldSpec_Phonetics;
    sp->flags |= Index_HasPhonetic;
  }

  RWLOCK_RELEASE();

  return fs;
}

void RediSearch_TextFieldSetWeight(IndexSpec* sp, FieldSpec* fs, double w) {
  assert(FIELD_IS(fs, INDEXFLD_T_FULLTEXT));
  fs->ftWeight = w;
}

void RediSearch_TagSetSeparator(FieldSpec* fs, char sep) {
  assert(FIELD_IS(fs, INDEXFLD_T_TAG));
  fs->tagSep = sep;
}

RSDoc* RediSearch_CreateDocument(const void* docKey, size_t len, double score, const char* lang) {
  RedisModuleString* docKeyStr = RedisModule_CreateString(NULL, docKey, len);
  const char* language = lang ? lang : "english";
  Document* ret = rm_calloc(1, sizeof(*ret));
  Document_Init(ret, docKeyStr, score, 0, language, NULL, 0);
  ret->language = strdup(ret->language);
  return ret;
}

int RediSearch_DeleteDocument(IndexSpec* sp, const void* docKey, size_t len) {
  RWLOCK_ACQUIRE_WRITE();

  RedisModuleString* docId = RedisModule_CreateString(NULL, docKey, len);
  int rc = REDISMODULE_OK;
  t_docId id = DocTable_GetIdR(&sp->docs, docId);
  if (id == 0) {
    rc = REDISMODULE_ERR;
  } else {
    rc = DocTable_DeleteR(&sp->docs, docId);
    if (rc) {
      sp->stats.numDocuments--;
    } else {
      // is this possible?
      rc = REDISMODULE_ERR;
    }
  }
  RedisModule_FreeString(NULL, docId);

  RWLOCK_RELEASE();
  return rc;
}

void RediSearch_DocumentAddField(Document* d, const char* fieldName, RedisModuleString* value,
                                 unsigned as) {
  Document_AddField(d, fieldName, value, as);
  RedisModule_RetainString(NULL, value);
}

void RediSearch_DocumentAddFieldString(Document* d, const char* fieldname, const char* s, size_t n,
                                       unsigned as) {
  RedisModuleString* r = RedisModule_CreateString(NULL, s, n);
  Document_AddField(d, fieldname, r, as);
}

void RediSearch_DocumentAddFieldNumber(Document* d, const char* fieldname, double n, unsigned as) {
  RedisModuleString* r = RedisModule_CreateStringPrintf(NULL, "%lf", n);
  Document_AddField(d, fieldname, r, as);
}

typedef struct {
  char** s;
  int hasErr;
} RSError;

void RediSearch_AddDocDone(RSAddDocumentCtx* aCtx, RedisModuleCtx* ctx, void* err) {
  RSError* ourErr = err;
  if (QueryError_HasError(&aCtx->status)) {
    if (ourErr->s) {
      *ourErr->s = strdup(QueryError_GetError(&aCtx->status));
    }
    ourErr->hasErr = aCtx->status.code;
  }
}

int RediSearch_IndexAddDocument(IndexSpec* sp, Document* d, int options, char** errs) {
  RWLOCK_ACQUIRE_WRITE();

  RSError err = {.s = errs};
  QueryError status = {0};
  RSAddDocumentCtx* aCtx = NewAddDocumentCtx(sp, d, &status);
  aCtx->donecb = RediSearch_AddDocDone;
  aCtx->donecbData = &err;
  RedisSearchCtx sctx = {.redisCtx = NULL, .spec = sp};
  int exists = !!DocTable_GetIdR(&sp->docs, d->docKey);
  if (exists) {
    if (options & REDISEARCH_ADD_REPLACE) {
      options |= DOCUMENT_ADD_REPLACE;
    } else {
      if (errs) {
        *errs = strdup("Document already exists");
      }
      AddDocumentCtx_Free(aCtx);
      RWLOCK_RELEASE();
      return REDISMODULE_ERR;
    }
  }

  options |= DOCUMENT_ADD_NOSAVE;
  aCtx->stateFlags |= ACTX_F_NOBLOCK;
  AddDocumentCtx_Submit(aCtx, &sctx, options);
  rm_free(d);

  RWLOCK_RELEASE();
  return err.hasErr ? REDISMODULE_ERR : REDISMODULE_OK;
}

QueryNode* RediSearch_CreateTokenNode(IndexSpec* sp, const char* fieldName, const char* token) {
  QueryNode* ret = NewQueryNode(QN_TOKEN);

  ret->tn = (QueryTokenNode){
      .str = (char*)strdup(token), .len = strlen(token), .expanded = 0, .flags = 0};
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RediSearch_CreateNumericNode(IndexSpec* sp, const char* field, double max, double min,
                                        int includeMax, int includeMin) {
  QueryNode* ret = NewQueryNode(QN_NUMERIC);
  ret->nn.nf = NewNumericFilter(min, max, includeMin, includeMax);
  ret->nn.nf->fieldName = strdup(field);
  ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, field, strlen(field));
  return ret;
}

QueryNode* RediSearch_CreatePrefixNode(IndexSpec* sp, const char* fieldName, const char* s) {
  QueryNode* ret = NewQueryNode(QN_PREFX);
  ret->pfx =
      (QueryPrefixNode){.str = (char*)strdup(s), .len = strlen(s), .expanded = 0, .flags = 0};
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RediSearch_CreateLexRangeNode(IndexSpec* sp, const char* fieldName, const char* begin,
                                         const char* end) {
  QueryNode* ret = NewQueryNode(QN_LEXRANGE);
  if (begin) {
    ret->lxrng.begin = begin;
  }
  if (end) {
    ret->lxrng.end = end;
  }
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RediSearch_CreateTagNode(IndexSpec* sp, const char* field) {
  QueryNode* ret = NewQueryNode(QN_TAG);
  ret->tag.fieldName = strdup(field);
  ret->tag.len = strlen(field);
  ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, field, strlen(field));
  return ret;
}

QueryNode* RediSearch_CreateIntersectNode(IndexSpec* sp, int exact) {
  QueryNode* ret = NewQueryNode(QN_PHRASE);
  ret->pn.exact = exact;
  return ret;
}

QueryNode* RediSearch_CreateUnionNode(IndexSpec* sp) {
  return NewQueryNode(QN_UNION);
}

QueryNode* RediSearch_CreateNotNode(IndexSpec* sp) {
  return NewQueryNode(QN_NOT);
}

int RediSearch_QueryNodeGetFieldMask(QueryNode* qn) {
  return qn->opts.fieldMask;
}

void RediSearch_QueryNodeAddChild(QueryNode* parent, QueryNode* child) {
  QueryNode_AddChild(parent, child);
}

void RediSearch_QueryNodeClearChildren(QueryNode* qn) {
  QueryNode_ClearChildren(qn, 1);
}

QueryNode* RediSearch_QueryNodeGetChild(const QueryNode* qn, size_t ix) {
  return QueryNode_GetChild(qn, ix);
}

size_t RediSearch_QueryNodeNumChildren(const QueryNode* qn) {
  return QueryNode_NumChildren(qn);
}

typedef struct RS_ApiIter {
  IndexIterator* internal;
  RSIndexResult* res;
  const RSDocumentMetadata* lastmd;
  ScoringFunctionArgs scargs;
  RSScoringFunction scorer;
  RSFreeFunction scorerFree;
  double minscore;  // Used for scoring
  QueryAST qast;    // Used for string queries..
} RS_ApiIter;

#define QUERY_INPUT_STRING 1
#define QUERY_INPUT_NODE 2

typedef struct {
  int qtype;
  union {
    struct {
      const char* qs;
      size_t n;
    } s;
    QueryNode* qn;
  } u;
} QueryInput;

static RS_ApiIter* handleIterCommon(IndexSpec* sp, QueryInput* input, char** error) {
  // here we only take the read lock and we will free it when the iterator will be freed
  RWLOCK_ACQUIRE_READ();

  RedisSearchCtx sctx = SEARCH_CTX_STATIC(NULL, sp);
  RSSearchOptions options = {0};
  QueryError status = {0};
  RSSearchOptions_Init(&options);
  RS_ApiIter* it = calloc(1, sizeof(*it));

  if (input->qtype == QUERY_INPUT_STRING) {
    if (QAST_Parse(&it->qast, &sctx, &options, input->u.s.qs, input->u.s.n, &status) !=
        REDISMODULE_OK) {
      goto end;
    }
  } else {
    it->qast.root = input->u.qn;
  }

  if (QAST_Expand(&it->qast, NULL, &options, &sctx, &status) != REDISMODULE_OK) {
    goto end;
  }

  it->internal = QAST_Iterate(&it->qast, &options, &sctx, NULL, &status);
  if (!it->internal) {
    goto end;
  }

  IndexSpec_GetStats(sp, &it->scargs.indexStats);
  ExtScoringFunctionCtx* scoreCtx = Extensions_GetScoringFunction(&it->scargs, DEFAULT_SCORER_NAME);
  assert(scoreCtx);
  it->scorer = scoreCtx->sf;
  it->scorerFree = scoreCtx->ff;
  it->minscore = DBL_MAX;

  // dummy statement for goto
  ;
end:
  if (input->qtype == QUERY_INPUT_NODE) {
    QueryNode_Free(it->qast.root);
    it->qast.root = NULL;
  }

  if (QueryError_HasError(&status) || it->internal == NULL) {
    if (it) {
      RediSearch_ResultsIteratorFree(it);
      it = NULL;
    }
    if (error) {
      *error = strdup(QueryError_GetError(&status));
    }
  }
  QueryError_ClearError(&status);
  return it;
}

RS_ApiIter* RediSearch_IterateQuery(IndexSpec* sp, const char* s, size_t n, char** error) {
  QueryInput input = {.qtype = QUERY_INPUT_STRING, .u = {.s = {s, .n = n}}};
  return handleIterCommon(sp, &input, error);
}

RS_ApiIter* RediSearch_GetResultsIterator(QueryNode* qn, IndexSpec* sp) {
  QueryInput input = {.qtype = QUERY_INPUT_NODE, .u = {.qn = qn}};
  return handleIterCommon(sp, &input, NULL);
}

void RediSearch_QueryNodeFree(QueryNode* qn) {
  QueryNode_Free(qn);
}

int RediSearch_QueryNodeType(QueryNode* qn) {
  return qn->type;
}

const void* RediSearch_ResultsIteratorNext(RS_ApiIter* iter, IndexSpec* sp, size_t* len) {
  while (iter->internal->Read(iter->internal->ctx, &iter->res) != INDEXREAD_EOF) {
    const RSDocumentMetadata* md = DocTable_Get(&sp->docs, iter->res->docId);
    if (md == NULL || ((md)->flags & Document_Deleted)) {
      continue;
    }
    iter->lastmd = md;
    if (len) {
      *len = sdslen(md->keyPtr);
    }
    return md->keyPtr;
  }
  return NULL;
}

double RediSearch_ResultsIteratorGetScore(const RS_ApiIter* it) {
  return it->scorer(&it->scargs, it->res, it->lastmd, 0);
}

void RediSearch_ResultsIteratorFree(RS_ApiIter* iter) {
  if (iter->internal) {
    iter->internal->Free(iter->internal);
  } else {
    printf("Not freeing internal iterator. internal iterator is null\n");
  }
  if (iter->scorerFree) {
    iter->scorerFree(iter->scargs.extdata);
  }
  QAST_Destroy(&iter->qast);
  rm_free(iter);

  RWLOCK_RELEASE();
}

void RediSearch_ResultsIteratorReset(RS_ApiIter* iter) {
  iter->internal->Rewind(iter->internal->ctx);
}

RSIndexOptions* RediSearch_CreateIndexOptions() {
  RSIndexOptions* ret = rm_calloc(1, sizeof(RSIndexOptions));
  ret->gcPolicy = GC_POLICY_NONE;
  return ret;
}

void RediSearch_FreeIndexOptions(RSIndexOptions* options) {
  rm_free(options);
}

void RediSearch_IndexOptionsSetGetValueCallback(RSIndexOptions* options, RSGetValueCallback cb,
                                                void* ctx) {
  options->gvcb = cb;
  options->gvcbData = ctx;
}

void RediSearch_IndexOptionsSetFlags(RSIndexOptions* options, uint32_t flags) {
  options->flags = flags;
}

void RediSearch_IndexOptionsSetGCPolicy(RSIndexOptions* options, int policy) {
  options->gcPolicy = policy;
}

#define REGISTER_API(name)                                                          \
  if (RedisModule_ExportSharedAPI(ctx, "RediSearch_" #name, RediSearch_##name) !=   \
      REDISMODULE_OK) {                                                             \
    RedisModule_Log(ctx, "warning", "could not register RediSearch_" #name "\r\n"); \
    return REDISMODULE_ERR;                                                         \
  }

int RediSearch_ExportCapi(RedisModuleCtx* ctx) {
  if (RedisModule_ExportSharedAPI == NULL) {
    RedisModule_Log(ctx, "warning", "Upgrade redis-server to use Redis Search's C API");
    return REDISMODULE_ERR;
  }
  RS_XAPIFUNC(REGISTER_API)
  return REDISMODULE_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief document collection with global read-write lock
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Copyright 2011, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "document-collection.h"

#include "BasicsC/conversions.h"
#include "BasicsC/files.h"
#include "BasicsC/hashes.h"
#include "BasicsC/logging.h"
#include "BasicsC/strings.h"
#include "ShapedJson/shape-accessor.h"
#include "VocBase/edge-collection.h"
#include "VocBase/index.h"
#include "VocBase/voc-shaper.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

static int CreateImmediateIndexes (TRI_document_collection_t*,
                                   TRI_doc_mptr_t*);

static int UpdateImmediateIndexes (TRI_document_collection_t*,
                                   TRI_doc_mptr_t const*,
                                   TRI_doc_mptr_t const*);

static int DeleteImmediateIndexes (TRI_document_collection_t*,
                                   TRI_doc_mptr_t const*,
                                   TRI_voc_tick_t);

static TRI_doc_mptr_t UpdateDocument (TRI_doc_operation_context_t*,
                                      TRI_doc_mptr_t const*,
                                      TRI_doc_document_key_marker_t*,
                                      TRI_voc_size_t,
                                      void const*,
                                      TRI_voc_size_t,
                                      void const*,
                                      TRI_voc_size_t,
                                      TRI_df_marker_t**);

static int DeleteDocument (TRI_doc_operation_context_t*,
                           TRI_doc_deletion_key_marker_t*,
                           void const*,
                           TRI_voc_size_t);

static int DeleteShapedJson (TRI_doc_operation_context_t*, 
                             TRI_voc_key_t);

static int CapConstraintFromJson (TRI_document_collection_t*,
                                  TRI_json_t*,
                                  TRI_idx_iid_t);

static int BitarrayIndexFromJson (TRI_document_collection_t*,
                                  TRI_json_t*,
                                  TRI_idx_iid_t);

static int GeoIndexFromJson (TRI_document_collection_t*,
                             TRI_json_t*,
                             TRI_idx_iid_t);

static int HashIndexFromJson (TRI_document_collection_t*,
                              TRI_json_t*,
                              TRI_idx_iid_t);

static int SkiplistIndexFromJson (TRI_document_collection_t*,
                                  TRI_json_t*,
                                  TRI_idx_iid_t);

static int PriorityQueueFromJson (TRI_document_collection_t*,
                                  TRI_json_t*,
                                  TRI_idx_iid_t);

// -----------------------------------------------------------------------------
// --SECTION--                                                  HELPER FUNCTIONS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief extracts the data length from a master pointer
////////////////////////////////////////////////////////////////////////////////

static size_t LengthDataMasterPointer (const TRI_doc_mptr_t* const mptr) {
  if (mptr != NULL) {
    void const* data = mptr->_data;

    if (((TRI_df_marker_t const*) data)->_type == TRI_DOC_MARKER_KEY_DOCUMENT) {      
      return ((TRI_df_marker_t*) data)->_size - ((TRI_doc_document_key_marker_t const*) data)->_offsetJson;
    }
    else if (((TRI_df_marker_t const*) data)->_type == TRI_DOC_MARKER_KEY_EDGE) {
      return ((TRI_df_marker_t*) data)->_size - ((TRI_doc_edge_key_marker_t const*) data)->base._offsetJson;
    }
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks whether a header is visible in the current context
////////////////////////////////////////////////////////////////////////////////
  
static bool IsVisible (TRI_doc_mptr_t const* header, 
                       const TRI_doc_operation_context_t* const context) {
  return (header != NULL && header->_validTo == 0);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                          JOURNALS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief selects a journal, possibly waits until a journal appears
///
/// Note that the function grabs a lock. We have to release this lock, in order
/// to allow the gc to start when waiting for a journal to appear.
////////////////////////////////////////////////////////////////////////////////

static TRI_datafile_t* SelectJournal (TRI_document_collection_t* document,
                                      TRI_voc_size_t size,
                                      TRI_df_marker_t** result) {
  TRI_datafile_t* datafile;
  TRI_collection_t* base;
  int res;
  size_t i;
  size_t n;
  
  base = &document->base.base;

  TRI_LOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  if (base->_maximumMarkerSize < size) {
    base->_maximumMarkerSize = size;
  }

  while (base->_state == TRI_COL_STATE_WRITE) {
    n = base->_journals._length;

    for (i = 0;  i < n;  ++i) {

      // select datafile
      datafile = base->_journals._buffer[i];

      // try to reserve space
      res = TRI_ReserveElementDatafile(datafile, size, result);

      // in case of full datafile, try next
      if (res == TRI_ERROR_NO_ERROR) {
        TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

        return datafile;
      }
      else if (res != TRI_ERROR_ARANGO_DATAFILE_FULL) {
        TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

        return NULL;
      }
    }

    TRI_INC_SYNCHRONISER_WAITER_VOC_BASE(base->_vocbase);
    TRI_WAIT_JOURNAL_ENTRIES_DOC_COLLECTION(document);
    TRI_DEC_SYNCHRONISER_WAITER_VOC_BASE(base->_vocbase);
  }

  TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief waits for synchronisation
///
/// Note that a datafile is never freed. If the datafile is closed the state
/// is set to TRI_DF_STATE_CLOSED - but the datafile pointer is still valid.
/// If a datafile is closed - then the data has been copied to some other
/// datafile and has been synced.
////////////////////////////////////////////////////////////////////////////////

static void WaitSync (TRI_document_collection_t* document,
                      TRI_datafile_t* journal,
                      char const* position) {
  TRI_collection_t* base;

  base = &document->base.base;

  // no condition at all. Do NOT acquire a lock, in the worst
  // case we will miss a parameter change.

  TRI_LOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  // wait until the sync condition is fullfilled
  while (true) {

    // check for error
    if (journal->_state == TRI_DF_STATE_WRITE_ERROR) {
      break;
    }

    // check for close
    if (journal->_state == TRI_DF_STATE_CLOSED) {
      break;
    }

    // always sync
    if (position <= journal->_synced) {
      break;
    }

    // we have to wait a bit longer
    // signal the synchroniser that there is work to do
    TRI_INC_SYNCHRONISER_WAITER_VOC_BASE(base->_vocbase);
    TRI_WAIT_JOURNAL_ENTRIES_DOC_COLLECTION(document);
    TRI_DEC_SYNCHRONISER_WAITER_VOC_BASE(base->_vocbase);
  }

  TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes data to the journal and updates the barriers
////////////////////////////////////////////////////////////////////////////////

static int WriteElement (TRI_document_collection_t* document,
                         TRI_datafile_t* journal,
                         TRI_df_marker_t* marker,
                         TRI_voc_size_t markerSize,
                         void const* keyBody,
                         TRI_voc_size_t keyBodySize,
                         void const* body,
                         TRI_voc_size_t bodySize,
                         TRI_df_marker_t* result) {
  int res;

  res = TRI_WriteElementDatafile(journal,
                                 result,
                                 marker, 
                                 markerSize,
                                 keyBody, 
                                 keyBodySize,
                                 body, 
                                 bodySize,
                                 false);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  TRI_LOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  journal->_written = ((char*) result) + marker->_size;
  journal->_nWritten++;

  TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                     DOCUMENT CRUD
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief compare revision of found document with revision specified in policy
/// this will also store the actual revision id found in the database in the
/// context variable _previousRid, but only if this is not NULL
////////////////////////////////////////////////////////////////////////////////

static int RevisionCheck (const TRI_doc_operation_context_t* const context,
                          const TRI_voc_rid_t actualRid) {
 
  // store previous revision 
  if (context->_previousRid != NULL) {
    *(context->_previousRid) = actualRid;
  }
 
  // check policy
  switch (context->_policy) {
    case TRI_DOC_UPDATE_ERROR:
      if (context->_expectedRid != 0 && context->_expectedRid != actualRid) {
        return TRI_ERROR_ARANGO_CONFLICT;
      }
      break;

    case TRI_DOC_UPDATE_CONFLICT:
      return TRI_ERROR_NOT_IMPLEMENTED;

    case TRI_DOC_UPDATE_ILLEGAL:
      return TRI_ERROR_INTERNAL;
    
    case TRI_DOC_UPDATE_LAST_WRITE:
      return TRI_ERROR_NO_ERROR;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write-unlock the collection if it was write-locked
////////////////////////////////////////////////////////////////////////////////

static void Unlock (const TRI_doc_operation_context_t* const context) {
  if (context->_release) {
    TRI_primary_collection_t* primary = context->_collection;

    primary->endWrite(primary);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new header
////////////////////////////////////////////////////////////////////////////////

static void CreateHeader (TRI_primary_collection_t* c,
                          TRI_datafile_t* datafile,
                          TRI_df_marker_t const* m,
                          size_t markerSize,
                          TRI_doc_mptr_t* header,
                          void const* additional) {
  TRI_doc_document_key_marker_t const* marker;

  marker = (TRI_doc_document_key_marker_t const*) m;

  header->_rid       = marker->_rid;
  header->_fid       = datafile->_fid;
  header->_validFrom = marker->_rid; // document creation time
  header->_validTo   = 0;            // document deletion time, 0 means "infinitely valid"
  header->_data      = marker;
  header->_key       = ((char*)marker) + marker->_offsetKey;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new document splitted into marker and body to file
////////////////////////////////////////////////////////////////////////////////

static int CreateDocument (TRI_doc_operation_context_t* context,
                           TRI_doc_document_key_marker_t* marker,
                           size_t markerSize,
                           void const* keyBody,
                           TRI_voc_size_t keyBodySize,
                           void const* body,
                           TRI_voc_size_t bodySize,
                           TRI_df_marker_t** result,
                           void const* additional,
                           TRI_doc_mptr_t* mptr) { 

  TRI_datafile_t* journal;
  TRI_primary_collection_t* primary;
  TRI_document_collection_t* document;
  TRI_doc_mptr_t* header;
  TRI_voc_size_t total;
  TRI_doc_datafile_info_t* dfi;
  int res;

  primary = context->_collection;
  document = (TRI_document_collection_t*) primary;

  // .............................................................................
  // create header
  // .............................................................................

  // get a new header pointer
  header = document->_headers->request(document->_headers);
  if (header == NULL) {
    Unlock(context);
    
    return TRI_ERROR_INTERNAL;
  }

  // find and select a journal
  total = markerSize + keyBodySize + bodySize;
  journal = SelectJournal(document, total, result);

  if (journal == NULL) {
    Unlock(context);

    return TRI_ERROR_INTERNAL;
  }

  // .............................................................................
  // write document blob
  // .............................................................................

  // verify the header pointer
  header = document->_headers->verify(document->_headers, header);

  // generate crc
  TRI_FillCrcKeyMarkerDatafile(&marker->base, markerSize, keyBody, keyBodySize, body, bodySize);

  // and write marker and blob
  res = WriteElement(document, journal, &marker->base, markerSize, keyBody, keyBodySize, body, bodySize, *result);

  if (res != TRI_ERROR_NO_ERROR) {
    Unlock(context);

    LOG_ERROR("cannot write element: %s", TRI_last_error());

    return res;
  }
  
  // .............................................................................
  // update indexes
  // .............................................................................

  // fill the header
  CreateHeader(primary, journal, *result, markerSize, header, 0);

  // update the datafile info
  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, journal->_fid);
  if (dfi != NULL) {
    dfi->_numberAlive += 1;
    dfi->_sizeAlive += LengthDataMasterPointer(header);
  }

  // update immediate indexes
  res = CreateImmediateIndexes(document, header);

  // check for constraint error, rollback if necessary
  if (res != TRI_ERROR_NO_ERROR) {
    TRI_doc_operation_context_t rollbackContext;
    int resRollback;

    LOG_DEBUG("encountered index violation during create, deleting newly created document");

    // rollback, ignore any additional errors
    TRI_InitContextPrimaryCollection(&rollbackContext, primary, TRI_DOC_UPDATE_LAST_WRITE, false);
    rollbackContext._expectedRid = marker->_rid;
    resRollback = DeleteShapedJson(&rollbackContext, (TRI_voc_key_t) keyBody);

    if (resRollback != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("encountered error '%s' during rollback of create", TRI_last_error());
    }

    Unlock(context);
    TRI_set_errno(res);

    return res;
  }

  // .............................................................................
  // create result
  // .............................................................................

  assert(res == TRI_ERROR_NO_ERROR);

  *mptr = *header;

  // check cap constraint
  if (primary->_capConstraint != NULL) {
    while (primary->_capConstraint->_size < primary->_capConstraint->_array._array._nrUsed) {
      TRI_doc_operation_context_t rollbackContext;
      TRI_doc_mptr_t const* oldest;
      int resRem;

      oldest = TRI_PopFrontLinkedArray(&primary->_capConstraint->_array);

      if (oldest == NULL) {
        LOG_WARNING("cap collection is empty, but collection '%ld' contains elements", 
            (unsigned long) primary->base._cid);
        break;
      }

      LOG_DEBUG("removing document '%s' because of cap constraint", (char*) oldest->_key);

      TRI_InitContextPrimaryCollection(&rollbackContext, primary, TRI_DOC_UPDATE_LAST_WRITE, false);
      resRem = DeleteShapedJson(&rollbackContext, oldest->_key);

      if (resRem != TRI_ERROR_NO_ERROR) {
        LOG_WARNING("cannot cap collection: %s", TRI_last_error());
        break;
      }
    }
  }
  
  Unlock(context);

  // wait for sync
  if (context->_sync) {
    WaitSync(document, journal, ((char const*) *result) + markerSize + keyBodySize + bodySize);
  }

  // and return
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates an existing header
////////////////////////////////////////////////////////////////////////////////

static void UpdateHeader (TRI_datafile_t* datafile,
                          TRI_df_marker_t const* m,
                          TRI_doc_mptr_t const* header,
                          TRI_doc_mptr_t* update) {
  TRI_doc_document_key_marker_t const* marker;

  marker = (TRI_doc_document_key_marker_t const*) m;
  *update = *header;

  update->_rid = marker->_rid;
  update->_fid = datafile->_fid;
  update->_data = marker;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief rolls back an update
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t RollbackUpdate (TRI_primary_collection_t* primary,
                                      TRI_doc_mptr_t const* header,
                                      TRI_df_marker_t const* originalMarker,
                                      TRI_df_marker_t** result) {
  TRI_doc_document_key_marker_t* marker;
  char* data;
  TRI_voc_size_t dataLength;
  TRI_voc_size_t markerLength;
  TRI_doc_operation_context_t rollbackContext;
  char* keyData = NULL;
  TRI_voc_size_t keyDataLength = 0;


  if (originalMarker->_type != TRI_DOC_MARKER_KEY_DOCUMENT &&
      originalMarker->_type != TRI_DOC_MARKER_KEY_EDGE) {
    // invalid marker type
    TRI_doc_mptr_t mptr;
    
    memset(&mptr, 0, sizeof(mptr));
    TRI_set_errno(TRI_ERROR_INTERNAL);
    LOG_WARNING("rollback operation called for unexpected marker type");

    return mptr;
  }


  if (originalMarker->_type == TRI_DOC_MARKER_KEY_DOCUMENT) {
    TRI_doc_document_key_marker_t documentUpdate;
    TRI_doc_document_key_marker_t* o = (TRI_doc_document_key_marker_t*) originalMarker;
    
    memcpy(&documentUpdate, originalMarker, sizeof(TRI_doc_document_key_marker_t));
    marker = &documentUpdate;
    markerLength = sizeof(TRI_doc_document_key_marker_t);
    
    keyData = ((char*) originalMarker) + o->_offsetKey;
    keyDataLength = o->_offsetJson - o->_offsetKey;
    
    data = ((char*) originalMarker) + marker->_offsetJson;
    dataLength = originalMarker->_size - marker->_offsetJson;
  }
  else if (originalMarker->_type == TRI_DOC_MARKER_KEY_EDGE) {
    TRI_doc_edge_key_marker_t edgeUpdate;
    TRI_doc_edge_key_marker_t* o = (TRI_doc_edge_key_marker_t*) originalMarker;

    memcpy(&edgeUpdate, originalMarker, sizeof(TRI_doc_edge_key_marker_t));
    marker = &edgeUpdate.base;       
    markerLength = sizeof(TRI_doc_edge_key_marker_t);
    
    keyData = ((char*) originalMarker) + o->base._offsetKey;
    keyDataLength = o->base._offsetJson - o->base._offsetKey;
    
    data = ((char*) originalMarker) + o->base._offsetJson;
    dataLength = originalMarker->_size - o->base._offsetJson;
  }

  // create a rollback context that does not rollback itself
  TRI_InitContextPrimaryCollection(&rollbackContext, primary, TRI_DOC_UPDATE_LAST_WRITE, false);
  rollbackContext._expectedRid = header->_rid;
  rollbackContext._allowRollback = false;

  return UpdateDocument(&rollbackContext,
                        header,
                        marker, 
                        markerLength,
                        keyData, 
                        keyDataLength,
                        data, 
                        dataLength,
                        result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates an existing document splitted into marker and body to file
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t UpdateDocument (TRI_doc_operation_context_t* context,
                                      TRI_doc_mptr_t const* header,
                                      TRI_doc_document_key_marker_t* marker,
                                      TRI_voc_size_t markerSize,
                                      void const* keyBody,
                                      TRI_voc_size_t keyBodySize,
                                      void const* body,
                                      TRI_voc_size_t bodySize,
                                      TRI_df_marker_t** result) {
  TRI_doc_mptr_t mptr;
  TRI_doc_mptr_t update;
  TRI_primary_collection_t* primary;
  TRI_document_collection_t* document;
  TRI_datafile_t* journal;
  TRI_df_marker_t const* originalMarker;
  TRI_doc_datafile_info_t* dfi;
  TRI_voc_size_t total;
  int res;

  primary = context->_collection;
  document = (TRI_document_collection_t*) primary;

  originalMarker = header->_data;
  memset(&mptr, 0, sizeof(mptr));

  // .............................................................................
  // check the revision
  // .............................................................................

  res = RevisionCheck(context, header->_rid);
  if (res != TRI_ERROR_NO_ERROR) {
    Unlock(context);
    TRI_set_errno(res);

    return mptr;
  }

  // .............................................................................
  // update header
  // .............................................................................

  // generate a new tick
  marker->_rid = marker->base._tick = TRI_NewTickVocBase();

  // find and select a journal
  total = markerSize + keyBodySize + bodySize;
  journal = SelectJournal(document, total, result);

  if (journal == NULL) {
    Unlock(context);
    primary->base._lastError = TRI_set_errno(TRI_ERROR_ARANGO_NO_JOURNAL);

    return mptr;
  }

  // .............................................................................
  // write document blob
  // .............................................................................

  // generate crc
  TRI_FillCrcMarkerDatafile(&marker->base, markerSize, keyBody, keyBodySize, body, bodySize);

  // and write marker and blob
  // TODO: update 
  res = WriteElement(document, journal, &marker->base, markerSize, keyBody, keyBodySize, body, bodySize, *result);
  
  if (res != TRI_ERROR_NO_ERROR) {
    Unlock(context);
    LOG_ERROR("cannot write element");

    return mptr;
  }

  // .............................................................................
  // update indexes
  // .............................................................................

  // update the header
  UpdateHeader(journal, *result, header, &update);

  // update the datafile info
  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, header->_fid);
  if (dfi != NULL) {
    size_t length = LengthDataMasterPointer(header);

    dfi->_numberAlive -= 1;
    dfi->_sizeAlive -= length;
    dfi->_numberDead += 1;
    dfi->_sizeDead += length;
  }

  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, journal->_fid);
  if (dfi != NULL) {
    dfi->_numberAlive += 1;
    dfi->_sizeAlive += LengthDataMasterPointer(&update);
  }

  // update immediate indexes
  res = UpdateImmediateIndexes(document, header, &update);

  // check for constraint error
  if (context->_allowRollback && res != TRI_ERROR_NO_ERROR) {
    TRI_doc_mptr_t resUpd;

    LOG_DEBUG("encountered index violating during update, rolling back");

    resUpd = RollbackUpdate(primary, header, originalMarker, result);
    if (resUpd._key == 0) {
      LOG_ERROR("encountered error '%s' during rollback of update", TRI_last_error());
    }
    TRI_set_errno(res);
  }

  // .............................................................................
  // create result
  // .............................................................................

  if (res == TRI_ERROR_NO_ERROR) {
    mptr = *header;
    
    Unlock(context);

    // wait for sync
    if (context->_sync) {
      WaitSync(document, journal, ((char const*) *result) + markerSize + bodySize);
    }
      
    // and return
    return mptr;
  }

  // error case
  assert(res != TRI_ERROR_NO_ERROR);
    
  Unlock(context);
  mptr._key = 0;

  return mptr;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an element and removes it from the index
////////////////////////////////////////////////////////////////////////////////

static int DeleteDocument (TRI_doc_operation_context_t* context,
                           TRI_doc_deletion_key_marker_t* marker,
                           void const* keyBody,
                           TRI_voc_size_t keyBodySize) {
  TRI_datafile_t* journal;
  TRI_df_marker_t* result;
  TRI_doc_mptr_t const* header;
  TRI_primary_collection_t* primary;
  TRI_document_collection_t* document;
  TRI_doc_datafile_info_t* dfi;
  TRI_voc_size_t total;
  int res;

  primary = context->_collection;
  document = (TRI_document_collection_t*) primary;

  // get an existing header pointer
  header = TRI_LookupByKeyAssociativePointer(&primary->_primaryIndex, keyBody);
  if (! IsVisible(header, context)) {
    Unlock(context);

    return TRI_set_errno(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
  }

  res = RevisionCheck(context, header->_rid);
  if (res != TRI_ERROR_NO_ERROR) {
    Unlock(context);

    return res;
  }

  // generate a new tick
  marker->base._tick = TRI_NewTickVocBase();

  // find and select a journal
  total = sizeof(TRI_doc_deletion_key_marker_t) + keyBodySize;
  journal = SelectJournal(document, total, &result);

  if (journal == NULL) {
    Unlock(context);

    primary->base._lastError = TRI_set_errno(TRI_ERROR_ARANGO_NO_JOURNAL);

    return TRI_ERROR_ARANGO_NO_JOURNAL;
  }

  // generate crc
  TRI_FillCrcMarkerDatafile(&marker->base, sizeof(TRI_doc_deletion_key_marker_t), keyBody, keyBodySize, 0, 0);

  // and write marker and blob
  res = WriteElement(document, journal, &marker->base, sizeof(TRI_doc_deletion_key_marker_t), keyBody, keyBodySize, 0, 0, result);

  if (res != TRI_ERROR_NO_ERROR) {
    Unlock(context);

    LOG_ERROR("cannot delete element");
  
    return res;
  }

  assert(res == TRI_ERROR_NO_ERROR);

  // update the datafile info
  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, header->_fid);
  if (dfi != NULL) {
    size_t length = LengthDataMasterPointer(header);

    dfi->_numberAlive -= 1;
    dfi->_sizeAlive -= length;

    dfi->_numberDead += 1;
    dfi->_sizeDead += length;
  }

  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, journal->_fid);
  if (dfi != NULL) {
    dfi->_numberDeletion += 1;
  }

  // update immediate indexes
  DeleteImmediateIndexes(document, header, marker->base._tick);

  Unlock(context);

  // wait for sync
  if (context->_sync) {
    WaitSync(document, journal, ((char const*) result) + sizeof(TRI_doc_deletion_key_marker_t) + keyBodySize);
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new document in the collection from json
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t CreateJson (TRI_doc_operation_context_t* context,
                                  TRI_df_marker_type_e type,
                                  TRI_json_t const* json,
                                  void const* data) {
  TRI_shaped_json_t* shaped;
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t result;
  TRI_voc_key_t key = 0;

  primary = context->_collection;

  shaped = TRI_ShapedJsonJson(primary->_shaper, json);

  if (shaped == 0) {
    Unlock(context);
    primary->base._lastError = TRI_set_errno(TRI_ERROR_ARANGO_SHAPER_FAILED);
    memset(&result, 0, sizeof(result));

    return result;
  }
  
  if (json != NULL && json->_type == TRI_JSON_ARRAY) {
    TRI_json_t* k = TRI_LookupArrayJson((TRI_json_t*) json, "_key");
    if (k != NULL && k->_type == TRI_JSON_STRING) {
      key = k->_value._string.data;
    }    
  }
  
  result = primary->create(context, type, shaped, data, key);

  TRI_FreeShapedJson(primary->_shaper, shaped);

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates a document in the collection from json
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t UpdateJson (TRI_doc_operation_context_t* context,
                                  TRI_json_t const* json,
                                  TRI_voc_key_t key) {
  TRI_shaped_json_t* shaped;
  TRI_doc_mptr_t result;
  TRI_primary_collection_t* primary;

  primary = context->_collection;

  shaped = TRI_ShapedJsonJson(primary->_shaper, json);

  if (shaped == 0) {
    Unlock(context);
    primary->base._lastError = TRI_set_errno(TRI_ERROR_ARANGO_SHAPER_FAILED);
    memset(&result, 0, sizeof(result));

    return result;
  }
  
  result = primary->update(context, shaped, key);
  
  TRI_FreeShapedJson(primary->_shaper, shaped);
  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               DOCUMENT COLLECTION
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief debug output for datafile information
////////////////////////////////////////////////////////////////////////////////

static void DebugDatafileInfoDatafile (TRI_primary_collection_t* primary,
                                       TRI_datafile_t* datafile) {
  TRI_doc_datafile_info_t* dfi;

  dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

  printf("DATAFILE '%s'\n", datafile->_filename);

  if (dfi == NULL) {
    printf(" no info\n\n");
    return;
  }

  printf("  number alive: %ld\n", (long) dfi->_numberAlive);
  printf("  size alive:   %ld\n", (long) dfi->_sizeAlive);
  printf("  number dead:  %ld\n", (long) dfi->_numberDead);
  printf("  size dead:    %ld\n", (long) dfi->_sizeDead);
  printf("  deletion:     %ld\n\n", (long) dfi->_numberDeletion);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief debug output for datafile information
////////////////////////////////////////////////////////////////////////////////

static void DebugDatafileInfoPrimaryCollection (TRI_primary_collection_t* collection) {
  TRI_datafile_t* datafile;
  size_t n;
  size_t i;

  // journals
  n = collection->base._journals._length;

  for (i = 0;  i < n;  ++i) {
    datafile = collection->base._journals._buffer[i];
    DebugDatafileInfoDatafile(collection, datafile);
  }

  // compactor journals
  n = collection->base._compactors._length;

  for (i = 0;  i < n;  ++i) {
    datafile = collection->base._compactors._buffer[i];
    DebugDatafileInfoDatafile(collection, datafile);
  }

  // datafiles
  n = collection->base._datafiles._length;

  for (i = 0;  i < n;  ++i) {
    datafile = collection->base._datafiles._buffer[i];
    DebugDatafileInfoDatafile(collection, datafile);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief debug output for datafile information
////////////////////////////////////////////////////////////////////////////////

static void DebugHeaderDocumentCollection (TRI_document_collection_t* collection) {
  TRI_primary_collection_t* primary;
  void** end;
  void** ptr;

  primary = &collection->base;

  ptr = primary->_primaryIndex._table;
  end = ptr + primary->_primaryIndex._nrAlloc;

  for (;  ptr < end;  ++ptr) {
    if (*ptr) {
      TRI_doc_mptr_t* d;

      d = *ptr;

      printf("fid %lu, key %s, rid %lu, validFrom:%lu validTo %lu\n",
             (unsigned long) d->_fid,
             (char*) d->_key,
             (unsigned long) d->_rid,
             (unsigned long) d->_validFrom,
             (unsigned long) d->_validTo);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise a document marker with common attributes
////////////////////////////////////////////////////////////////////////////////
    
static void InitDocumentMarker (TRI_doc_document_key_marker_t* marker, 
                                const TRI_df_marker_type_t type,
                                TRI_shaped_json_t const* json,
                                const bool generateRid) {
  marker->base._type = type; 

  // generate a new tick
  if (generateRid) {
    marker->_rid = marker->base._tick = TRI_NewTickVocBase();
  }

  marker->_sid = 0;
  marker->_shape = json->_sid;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new document in the collection from shaped json
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t CreateShapedJson (TRI_doc_operation_context_t* context,
                                        TRI_df_marker_type_e type,
                                        TRI_shaped_json_t const* json,
                                        void const* data,
                                        char* key) {
  TRI_df_marker_t* result;
  TRI_primary_collection_t* primary;
  TRI_document_collection_t* document;
  size_t keySize = 0;
  char* keyBody = 0;
  TRI_voc_size_t keyBodySize = 0; 
  char ridBuffer[33];  
  size_t fromSize = 0;
  size_t toSize = 0;
  TRI_doc_mptr_t mptr;

  // initialise the result
  memset(&mptr, 0, sizeof(mptr));
  
  if (type != TRI_DOC_MARKER_KEY_DOCUMENT && 
      type != TRI_DOC_MARKER_KEY_EDGE) {
    // invalid marker type
    Unlock(context);

    LOG_FATAL("unknown marker type %lu", (unsigned long) type);
    TRI_FlushLogging();
    exit(EXIT_FAILURE);
  }
 

  primary = context->_collection;

  if (key) {
    document = (TRI_document_collection_t*) primary;

    // check key
    if (regexec(&document->DocumentKeyRegex, key, 0, NULL, 0) != 0 || strlen(key) > document->keyLength) {
      Unlock(context);
      primary->base._lastError = TRI_set_errno(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);

      return mptr;
    }
  }
  
  // type & key are valid
  
  if (type == TRI_DOC_MARKER_KEY_DOCUMENT) {
    // create a document
    TRI_doc_document_key_marker_t marker;

    memset(&marker, 0, sizeof(marker));
    InitDocumentMarker(&marker, TRI_DOC_MARKER_KEY_DOCUMENT, json, true);
   
    if (key) {
      // we have a key!
      keySize = strlen(key) + 1;
      keyBodySize = ((keySize + TRI_DF_BLOCK_ALIGN - 1) / TRI_DF_BLOCK_ALIGN) * TRI_DF_BLOCK_ALIGN;
      keyBody = TRI_Allocate(TRI_CORE_MEM_ZONE, keyBodySize, true);
      TRI_CopyString(keyBody, key, keySize);      
    }
    else {
      // create key from did      
      TRI_StringUInt64InPlace(marker._rid, ridBuffer);      
      keySize = strlen(ridBuffer) + 1;      
      keyBodySize = ((keySize + TRI_DF_BLOCK_ALIGN - 1) / TRI_DF_BLOCK_ALIGN) * TRI_DF_BLOCK_ALIGN;
      keyBody = TRI_Allocate(TRI_CORE_MEM_ZONE, keyBodySize, true);
      TRI_CopyString(keyBody, ridBuffer, keySize);
    }

    marker._offsetKey = sizeof(marker);
    marker._offsetJson = sizeof(marker) + keyBodySize;
    
    marker.base._size = sizeof(marker) + json->_data.length + keyBodySize;

    CreateDocument(context,
                   &marker, 
                   sizeof(marker),
                   keyBody, 
                   keyBodySize, 
                   json->_data.data, 
                   json->_data.length,
                   &result,
                   data,
                   &mptr);
  }
  else {
    // create an edge
    TRI_doc_edge_key_marker_t marker;
    TRI_document_edge_t const* edge;

    edge = data;

    memset(&marker, 0, sizeof(marker));
    InitDocumentMarker(&marker.base, TRI_DOC_MARKER_KEY_EDGE, json, true);

    marker._fromCid = edge->_fromCid;
    marker._toCid = edge->_toCid;
    marker._isBidirectional = (uint8_t) edge->_isBidirectional;

    fromSize = strlen(edge->_fromKey) + 1;    
    toSize = strlen(edge->_toKey) + 1;        
    
    if (key) {
      // we have a key!
      keySize = strlen(key)+1;
      keyBodySize = ((keySize + fromSize + toSize + TRI_DF_BLOCK_ALIGN - 1) / TRI_DF_BLOCK_ALIGN) * TRI_DF_BLOCK_ALIGN;
      keyBody = TRI_Allocate(TRI_CORE_MEM_ZONE, keyBodySize, true);
      TRI_CopyString(keyBody, key, keySize);      
    }
    else {
      // create key from did
      TRI_StringUInt64InPlace(marker.base._rid, ridBuffer);      
      keySize = strlen(ridBuffer) + 1;
      keyBodySize = ((keySize + fromSize + toSize + TRI_DF_BLOCK_ALIGN - 1) / TRI_DF_BLOCK_ALIGN) * TRI_DF_BLOCK_ALIGN;
      keyBody = TRI_Allocate(TRI_CORE_MEM_ZONE, keyBodySize, true);
      TRI_CopyString(keyBody, ridBuffer, keySize);      
    }

    TRI_CopyString((keyBody + keySize),          edge->_toKey, toSize);      
    TRI_CopyString((keyBody + keySize + toSize), edge->_fromKey, fromSize);      
    
    marker.base._offsetKey = sizeof(marker);
    marker.base._offsetJson = sizeof(marker) + keyBodySize;                
    marker._offsetToKey =  marker.base._offsetKey + keySize;
    marker._offsetFromKey = marker._offsetToKey + toSize;
    
    marker.base.base._size = sizeof(marker) + keyBodySize + json->_data.length;
    
    CreateDocument(context,
                   &marker.base, 
                   sizeof(marker),
                   keyBody, 
                   keyBodySize, 
                   json->_data.data, 
                   json->_data.length,
                   &result,
                   data,
                   &mptr);
  }
  
  TRI_FreeString(TRI_CORE_MEM_ZONE, keyBody);
  
  return mptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads an element from the document collection
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t ReadShapedJson (TRI_doc_operation_context_t* context,
                                      TRI_voc_key_t key) {
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t const* header;

  primary = context->_collection; 

  header = TRI_LookupByKeyAssociativePointer(&primary->_primaryIndex, key);

  if (! IsVisible(header, context)) {
    TRI_doc_mptr_t result;

    memset(&result, 0, sizeof(result));

    return result;
  }

  return *header;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates a document in the collection from shaped json
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t UpdateShapedJson (TRI_doc_operation_context_t* context,
                                        TRI_shaped_json_t const* json,
                                        TRI_voc_key_t key) {
  TRI_df_marker_t const* original;
  TRI_df_marker_t* result;
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t mptr;
  TRI_doc_mptr_t const* header;
  char* keyBody = NULL;
  size_t keyBodyLength = 0;         
  
  // initialise the result
  memset(&mptr, 0, sizeof(mptr));

  primary = context->_collection; 

  // get an existing header pointer
  header = TRI_LookupByKeyAssociativePointer(&primary->_primaryIndex, key);

  if (! IsVisible(header, context)) {
    Unlock(context);
    TRI_set_errno(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);

    return mptr;
  }

  original = header->_data;

  if (original->_type != TRI_DOC_MARKER_KEY_DOCUMENT &&
      original->_type != TRI_DOC_MARKER_KEY_EDGE) {
    // invalid marker type
    Unlock(context);

    LOG_FATAL("unknown marker type %lu", (unsigned long) original->_type);
    TRI_FlushLogging();
    exit(EXIT_FAILURE);
  }


  if (original->_type == TRI_DOC_MARKER_KEY_DOCUMENT) {
    // the original is a document
    TRI_doc_document_key_marker_t marker;
    TRI_doc_document_key_marker_t const* o;
    o = header->_data;
            
    // create an update
    memset(&marker, 0, sizeof(marker));
    InitDocumentMarker(&marker, o->base._type, json, false);
    
    keyBody = ((char*) original) + o->_offsetKey;  
    keyBodyLength = o->_offsetJson - o->_offsetKey;
    
    marker._offsetJson = o->_offsetJson;
    marker._offsetKey = o->_offsetKey;
    
    marker.base._size = sizeof(marker) + keyBodyLength + json->_data.length;    
    
    return UpdateDocument(context,
                          header,
                          &marker, 
                          sizeof(marker),
                          keyBody,  
                          keyBodyLength,
                          json->_data.data, 
                          json->_data.length,
                          &result);
  }

  // the original is an edge
  else {
    TRI_doc_edge_key_marker_t marker;
    TRI_doc_edge_key_marker_t const* o;

    o = header->_data;

    // create an update
    memset(&marker, 0, sizeof(marker));
    InitDocumentMarker(&marker.base, o->base.base._type, json, false);

    marker._fromCid = o->_fromCid;
    marker._toCid = o->_toCid;
    marker._isBidirectional = o->_isBidirectional;

    keyBody = ((char*) o) + o->base._offsetKey;  
    keyBodyLength = o->base._offsetJson - o->base._offsetKey;
    
    marker.base._offsetJson = o->base._offsetJson;
    marker.base._offsetKey = o->base._offsetKey;
    marker._offsetFromKey = o->_offsetFromKey;
    marker._offsetToKey = o->_offsetToKey;
    
    marker.base.base._size = sizeof(marker) + keyBodyLength + json->_data.length;    
    
    return UpdateDocument(context,
                          header,
                          &marker.base, 
                          sizeof(marker),
                          keyBody,  
                          keyBodyLength,
                          json->_data.data, 
                          json->_data.length,
                          &result);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes a json document given the identifier
////////////////////////////////////////////////////////////////////////////////

static int DeleteShapedJson (TRI_doc_operation_context_t* context,
                             TRI_voc_key_t key) {
  TRI_doc_deletion_key_marker_t marker;
  TRI_voc_size_t keyBodySize = 0;

  memset(&marker, 0, sizeof(marker));
  marker.base._type = TRI_DOC_MARKER_KEY_DELETION;
  marker._sid = 0;

  if (key) {
    keyBodySize = strlen(key) + 1;
  }
  
  marker._offsetKey = sizeof(marker);
  marker.base._size = sizeof(marker) + keyBodySize;
  
  return DeleteDocument(context, &marker, key, keyBodySize);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read locks a collection
////////////////////////////////////////////////////////////////////////////////

static int BeginRead (TRI_primary_collection_t* primary) {
  TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read unlocks a collection
////////////////////////////////////////////////////////////////////////////////

static int EndRead (TRI_primary_collection_t* primary) {
  TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write locks a collection
////////////////////////////////////////////////////////////////////////////////

static int BeginWrite (TRI_primary_collection_t* primary) {
  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write unlocks a collection
////////////////////////////////////////////////////////////////////////////////

static int EndWrite (TRI_primary_collection_t* primary) {
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               DOCUMENT COLLECTION
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief iterator for open
////////////////////////////////////////////////////////////////////////////////

static bool OpenIterator (TRI_df_marker_t const* marker, void* data, TRI_datafile_t* datafile, bool journal) {
  TRI_document_collection_t* collection = data;
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t const* found;
  TRI_doc_datafile_info_t* dfi;
  TRI_voc_key_t key = NULL;
  
  primary = &collection->base;

  // new or updated document
  if (marker->_type == TRI_DOC_MARKER_KEY_EDGE ||
      marker->_type == TRI_DOC_MARKER_KEY_DOCUMENT) {
    TRI_doc_document_key_marker_t const* d = (TRI_doc_document_key_marker_t const*) marker;
    size_t markerSize;

    if (marker->_type == TRI_DOC_MARKER_KEY_DOCUMENT) {

      LOG_TRACE("document: fid %lu, key %s, rid %lu, _offsetJson %lu, _offsetKey %lu",
                (unsigned long) datafile->_fid,
                ((char*) d + d->_offsetKey),
                (unsigned long) d->_rid,
                (unsigned long) d->_offsetJson,
                (unsigned long) d->_offsetKey);
      
      markerSize = sizeof(TRI_doc_document_key_marker_t);
      key = ((char*) d) + d->_offsetKey;
    }
    else {
      TRI_doc_edge_key_marker_t const* e = (TRI_doc_edge_key_marker_t const*) marker;

      LOG_TRACE("edge: fid %lu, key %s, fromKey %s, toKey %s, rid %lu, _offsetJson %lu, _offsetKey %lu",
                (unsigned long) datafile->_fid,
                ((char*) d + d->_offsetKey),
                ((char*) e + e->_offsetFromKey),
                ((char*) e + e->_offsetToKey),
                (unsigned long) d->_rid,
                (unsigned long) d->_offsetJson,
                (unsigned long) d->_offsetKey);
      
      markerSize = sizeof(TRI_doc_edge_key_marker_t);
      key = ((char*) d) + d->_offsetKey;
    }

    if (primary->base._maximumMarkerSize < markerSize) {
      primary->base._maximumMarkerSize = markerSize;
    }

    found = TRI_LookupByKeyAssociativePointer(&primary->_primaryIndex, key);

    // it is a new entry
    if (found == NULL) {
      TRI_doc_mptr_t* header;

      header = collection->_headers->request(collection->_headers);
      // TODO: header might be NULL and must be checked
      header = collection->_headers->verify(collection->_headers, header);

      // fill the header
      CreateHeader(primary, datafile, marker, markerSize, header, 0);

      // update the datafile info
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

      if (dfi != NULL) {
        dfi->_numberAlive += 1;
        dfi->_sizeAlive += LengthDataMasterPointer(header);
      }

      // update immediate indexes
      CreateImmediateIndexes(collection, header);
    }

    // it is a delete
    else if (found->_validTo != 0) {
      // TODO: fix for trx: check if delete was committed or not
      LOG_TRACE("skipping already deleted document: %s", key);
    }

    // it is an update, but only if found has a smaller revision identifier
    else if (found->_rid < d->_rid || (found->_rid == d->_rid && found->_fid <= datafile->_fid)) {
      TRI_doc_mptr_t update;

      // update the header info
      UpdateHeader(datafile, marker, found, &update);

      // update the datafile info
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, found->_fid);

      if (dfi != NULL) {
        size_t length = LengthDataMasterPointer(found);

        dfi->_numberAlive -= 1;
        dfi->_sizeAlive -= length;

        dfi->_numberDead += 1;
        dfi->_sizeDead += length;
      }

      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

      if (dfi != NULL) {
        dfi->_numberAlive += 1;
        dfi->_sizeAlive += LengthDataMasterPointer(&update);
      }

      // update immediate indexes
      UpdateImmediateIndexes(collection, found, &update);
    }

    // it is a stale update
    else {
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

      if (dfi != NULL) {
        dfi->_numberDead += 1;
        dfi->_sizeDead += LengthDataMasterPointer(found);
      }
    }
  }
  // deletion
  else if (marker->_type == TRI_DOC_MARKER_KEY_DELETION) {
    TRI_doc_deletion_key_marker_t const* d;

    d = (TRI_doc_deletion_key_marker_t const*) marker;
    key = ((char*) d) + d->_offsetKey;

    LOG_TRACE("deletion: fid %lu, key %s, rid %lu, deletion %lu",
              (unsigned long) datafile->_fid,
              (char*) key,
              (unsigned long) d->_rid,
              (unsigned long) marker->_tick);

    found = TRI_LookupByKeyAssociativePointer(&primary->_primaryIndex, key);

    // it is a new entry, so we missed the create
    if (found == NULL) {
      TRI_doc_mptr_t* header;

      header = collection->_headers->request(collection->_headers);
      // TODO: header might be NULL and must be checked
      header = collection->_headers->verify(collection->_headers, header);

      header->_rid = d->_rid;
      header->_validFrom = marker->_tick;
      header->_validTo   = marker->_tick; // TODO: fix for trx
      header->_data = 0;
      header->_key = key;

      // update immediate indexes
      CreateImmediateIndexes(collection, header);

      // update the datafile info
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

      if (dfi != NULL) {
        dfi->_numberDeletion += 1;
      }
    }

    // it is a real delete
    else if (found->_validTo == 0) {
      union { TRI_doc_mptr_t const* c; TRI_doc_mptr_t* v; } change;

      // mark element as deleted
      change.c = found;
      change.v->_validFrom = marker->_tick;
      change.v->_validTo   = marker->_tick; // TODO: fix for trx

      // update the datafile info
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, found->_fid);

      if (dfi != NULL) {
        size_t length = LengthDataMasterPointer(found);

        dfi->_numberAlive -= 1;
        dfi->_sizeAlive -= length;

        dfi->_numberDead += 1;
        dfi->_sizeDead += length; 
      }
      dfi = TRI_FindDatafileInfoPrimaryCollection(primary, datafile->_fid);

      if (dfi != NULL) {
        dfi->_numberDeletion += 1;
      }
    }

    // it is a double delete
    else {
      LOG_TRACE("skipping deletion of already deleted document: %s", (char*) key);
    }
  }
  else {
    LOG_TRACE("skipping marker %lu", (unsigned long) marker->_type);
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief iterator for index open
////////////////////////////////////////////////////////////////////////////////

static bool OpenIndexIterator (char const* filename, void* data) {
  TRI_idx_iid_t iid;
  TRI_json_t* iis;
  TRI_json_t* json;
  TRI_json_t* type;
  TRI_document_collection_t* document;
  char const* typeStr;
  char* error;
  int res;

  // load json description of the index
  json = TRI_JsonFile(TRI_CORE_MEM_ZONE, filename, &error);

  // json must be a index description
  if (json == NULL) {
    LOG_ERROR("cannot read index definition from '%s': %s", filename, error);
    return false;
  }

  if (json->_type != TRI_JSON_ARRAY) {
    LOG_ERROR("cannot read index definition from '%s': expecting an array", filename);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return false;
  }

  // extract the type
  type = TRI_LookupArrayJson(json, "type");

  if (type->_type != TRI_JSON_STRING) {
    LOG_ERROR("cannot read index definition from '%s': expecting a string for type", filename);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return false;
  }

  typeStr = type->_value._string.data;

  // extract the index identifier
  iis = TRI_LookupArrayJson(json, "id");

  if (iis != NULL && iis->_type == TRI_JSON_NUMBER) {
    iid = iis->_value._number;
    TRI_UpdateTickVocBase(iid);
  }
  else {
    LOG_ERROR("ignoring index, index identifier could not be located");
    return false;
  }
  
  // document collection of the index
  document = (TRI_document_collection_t*) data;

  // ...........................................................................
  // CAP CONSTRAINT
  // ...........................................................................

  if (TRI_EqualString(typeStr, "cap")) {
    res = CapConstraintFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }

  
  // ...........................................................................
  // BITARRAY INDEX
  // ...........................................................................

  else if (TRI_EqualString(typeStr, "bitarray")) {
    res = BitarrayIndexFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }
  
  // ...........................................................................
  // GEO INDEX (list or attribute)
  // ...........................................................................

  else if (TRI_EqualString(typeStr, "geo1") || TRI_EqualString(typeStr, "geo2")) {
    res = GeoIndexFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }
  
  // ...........................................................................
  // HASH INDEX
  // ...........................................................................

  else if (TRI_EqualString(typeStr, "hash")) {
    res = HashIndexFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }

  // ...........................................................................
  // SKIPLIST INDEX
  // ...........................................................................

  else if (TRI_EqualString(typeStr, "skiplist")) {
    res = SkiplistIndexFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }

  // ...........................................................................
  // PRIORITY QUEUE
  // ...........................................................................

  else if (TRI_EqualString(typeStr, "priorityqueue")) {
    res = PriorityQueueFromJson(document, json, iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return res == TRI_ERROR_NO_ERROR;
  }

  // .........................................................................
  // oops, unknown index type
  // .........................................................................

  else {
    LOG_ERROR("ignoring unknown index type '%s' for index %lu", 
              typeStr,
              (unsigned long) iid);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    return false;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a document collection
////////////////////////////////////////////////////////////////////////////////

static bool InitDocumentCollection (TRI_document_collection_t* collection,
                                    TRI_shaper_t* shaper) {
  TRI_index_t* primary;
  char* expr;
  
  TRI_InitPrimaryCollection(&collection->base, shaper);
 
  collection->_headers = TRI_CreateSimpleHeaders(sizeof(TRI_doc_mptr_t));
  if (collection->_headers == NULL) {
    TRI_DestroyPrimaryCollection(&collection->base);

    return false;
  }

  if (collection->base.base._type == TRI_COL_TYPE_EDGE) {
    TRI_InitEdgesDocumentCollection(collection);
  }

  // create primary index 
  TRI_InitVectorPointer(&collection->_allIndexes, TRI_UNKNOWN_MEM_ZONE);

  primary = TRI_CreatePrimaryIndex(&collection->base);
  if (primary == NULL) {
    TRI_DestroyVectorPointer(&collection->_allIndexes);
    TRI_DestroyPrimaryCollection(&collection->base);

    return false;
  }
  TRI_PushBackVectorPointer(&collection->_allIndexes, primary);
  
  TRI_InitCondition(&collection->_journalsCondition);

  // setup methods
  collection->base.beginRead  = BeginRead;
  collection->base.endRead    = EndRead;

  collection->base.beginWrite = BeginWrite;
  collection->base.endWrite   = EndWrite;

  collection->base.create     = CreateShapedJson;
  collection->base.createJson = CreateJson;
  collection->base.read       = ReadShapedJson;
  collection->base.update     = UpdateShapedJson;
  collection->base.updateJson = UpdateJson;
  collection->base.destroy    = DeleteShapedJson;
  
  expr = "^[0-9a-zA-Z][_0-9a-zA-Z]*$";
  if (regcomp(&collection->DocumentKeyRegex, expr, REG_ICASE | REG_EXTENDED) != 0) {
    LOG_FATAL("cannot compile regular expression");
    TRI_FlushLogging();
    exit(EXIT_FAILURE);
  }
  
  collection->keyLength = 200;
  
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* TRI_CreateDocumentCollection (TRI_vocbase_t* vocbase,
                                                         char const* path,
                                                         TRI_col_parameter_t* parameter,
                                                         TRI_voc_cid_t cid) {
  TRI_col_info_t info;
  TRI_collection_t* collection;
  TRI_shaper_t* shaper;
  TRI_document_collection_t* document;
  bool waitForSync;

  memset(&info, 0, sizeof(info));
  info._version = TRI_COL_VERSION;
  info._type = parameter->_type;

  if (cid > 0) {
    TRI_UpdateTickVocBase(cid);
  }
  else {
    cid = TRI_NewTickVocBase();
  }
  info._cid = cid;
  TRI_CopyString(info._name, parameter->_name, sizeof(info._name));
  info._waitForSync = parameter->_waitForSync;
  info._maximalSize = parameter->_maximalSize;

  // first create the document collection
  document = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_document_collection_t), false);

  if (document == NULL) {
    LOG_ERROR("cannot create document collection");
    return NULL;
  }

  collection = TRI_CreateCollection(vocbase, &document->base.base, path, &info);

  if (collection == NULL) {
    LOG_ERROR("cannot create document collection");

    TRI_Free(TRI_UNKNOWN_MEM_ZONE, document);
    return NULL;
  }

  // then the shape collection
  waitForSync = (vocbase->_forceSyncShapes || parameter->_waitForSync);
  shaper = TRI_CreateVocShaper(vocbase, collection->_directory, "SHAPES", waitForSync);

  if (shaper == NULL) {
    LOG_ERROR("cannot create shapes collection");

    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection); // will free document

    return NULL;
  }

  // create document collection and shaper
  if (false == InitDocumentCollection(document, shaper)) {
    LOG_ERROR("cannot initialise shapes collection");

    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection); // will free document

    return NULL;
  }

  return document;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the memory allocated, but does not free the pointer
///
/// Note that the collection must be closed first.
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyDocumentCollection (TRI_document_collection_t* collection) {
  size_t i;
  size_t n;

  TRI_DestroyCondition(&collection->_journalsCondition);

  // only required for edge collections
  if (collection->base.base._type == TRI_COL_TYPE_EDGE) {
    TRI_FreeEdgesDocumentCollection(collection);
  }

  TRI_FreeSimpleHeaders(collection->_headers);

  // free memory allocated for index field names
  n = collection->_allIndexes._length;
  for (i = 0 ; i < n ; ++i) {
    TRI_index_t* idx = (TRI_index_t*) collection->_allIndexes._buffer[i];
  
    TRI_FreeIndex(idx);
  }
  // free index vector
  TRI_DestroyVectorPointer(&collection->_allIndexes);

  TRI_DestroyPrimaryCollection(&collection->base);
  
  regfree(&collection->DocumentKeyRegex);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the memory allocated and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeDocumentCollection (TRI_document_collection_t* collection) {
  TRI_DestroyDocumentCollection(collection);
  TRI_Free(TRI_UNKNOWN_MEM_ZONE, collection);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new journal
////////////////////////////////////////////////////////////////////////////////

TRI_datafile_t* TRI_CreateJournalDocumentCollection (TRI_document_collection_t* collection) {
  return TRI_CreateJournalPrimaryCollection(&collection->base);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes an existing journal
////////////////////////////////////////////////////////////////////////////////

bool TRI_CloseJournalDocumentCollection (TRI_document_collection_t* collection,
                                         size_t position) {
  return TRI_CloseJournalPrimaryCollection(&collection->base, position);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief opens an existing collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* TRI_OpenDocumentCollection (TRI_vocbase_t* vocbase, char const* path) {
  TRI_collection_t* collection;
  TRI_shaper_t* shaper;
  TRI_document_collection_t* document;
  TRI_shape_collection_t* shapeCollection;
  char* shapes;

  // first open the document collection
  document = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_document_collection_t), false);
  if (document == NULL) {
    return NULL;
  }

  collection = TRI_OpenCollection(vocbase, &document->base.base, path);

  if (collection == NULL) {
    LOG_ERROR("cannot open document collection from path '%s'", path);

    TRI_Free(TRI_UNKNOWN_MEM_ZONE, document);
    return NULL;
  }

  // then the shape collection
  shapes = TRI_Concatenate2File(collection->_directory, "SHAPES");
  if (!shapes) {
    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection);
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, document);
    return NULL;
  }

  shaper = TRI_OpenVocShaper(vocbase, shapes);
  TRI_FreeString(TRI_CORE_MEM_ZONE, shapes);

  if (shaper == NULL) {
    LOG_ERROR("cannot open shapes collection");

    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection);

    return NULL;
  }

  // create document collection and shaper
  if (false == InitDocumentCollection(document, shaper)) {
    LOG_ERROR("cannot initialise document collection");

    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection);

    return NULL;
  }
  
  assert(shaper);
  shapeCollection = TRI_CollectionVocShaper(shaper);
  if (shapeCollection != NULL) {
    shapeCollection->base._waitForSync = (vocbase->_forceSyncShapes || collection->_waitForSync);
  }


  // read all documents and fill indexes
  TRI_IterateCollection(collection, OpenIterator, collection);

  if (collection->_maximalSize < collection->_maximumMarkerSize + TRI_JOURNAL_OVERHEAD) {
    LOG_WARNING("maximal size is %lu, but maximal marker size is %lu plus overhead %lu: adjusting maximal size to %lu",
                (unsigned long) collection->_maximalSize,
                (unsigned long) collection->_maximumMarkerSize,
                (unsigned long) TRI_JOURNAL_OVERHEAD,
                (unsigned long) (collection->_maximumMarkerSize + TRI_JOURNAL_OVERHEAD));

    collection->_maximalSize = collection->_maximumMarkerSize + TRI_JOURNAL_OVERHEAD;
  }

  TRI_IterateIndexCollection(collection, OpenIndexIterator, collection);

  // output infomations about datafiles and journals
  if (TRI_IsTraceLogging(__FILE__)) {
    DebugDatafileInfoPrimaryCollection(&document->base);
    DebugHeaderDocumentCollection(document);
  }

  return document;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes an open collection
////////////////////////////////////////////////////////////////////////////////

int TRI_CloseDocumentCollection (TRI_document_collection_t* collection) {
  int res;

  res = TRI_CloseCollection(&collection->base.base);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  res = TRI_CloseVocShaper(collection->base._shaper);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // this does also destroy the shaper's underlying blob collection
  TRI_FreeVocShaper(collection->base._shaper);

  collection->base._shaper = NULL;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                           INDEXES
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                     private types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief pid name structure
////////////////////////////////////////////////////////////////////////////////

typedef struct pid_name_s {
  TRI_shape_pid_t _pid;
  char* _name;
}
pid_name_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief converts extracts a field list from a json object
////////////////////////////////////////////////////////////////////////////////

static TRI_json_t* ExtractFields (TRI_json_t* json, size_t* fieldCount, TRI_idx_iid_t iid) {
  TRI_json_t* fld;
  size_t j;

  fld = TRI_LookupArrayJson(json, "fields");

  if (fld == NULL || fld->_type != TRI_JSON_LIST) {
    LOG_ERROR("ignoring index %lu, 'fields' must be a list", (unsigned long) iid);

    TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    return NULL;
  }

  *fieldCount = fld->_value._objects._length;

  for (j = 0;  j < *fieldCount;  ++j) {
    TRI_json_t* sub = TRI_AtVector(&fld->_value._objects, j);
      
    if (sub->_type != TRI_JSON_STRING) {
      LOG_ERROR("ignoring index %lu, 'fields' must be a list of attribute paths", (unsigned long) iid);

      TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
      return NULL;
    }
  }

  return fld;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the list of attribute/value pairs
///
/// Attribute/value pairs are used in the construction of static bitarray 
/// indexes. These pairs are stored in a json object from which they can be
/// later extracted. Here is the extraction function given the index definition
/// as a json object
////////////////////////////////////////////////////////////////////////////////

static TRI_json_t* ExtractFieldValues (TRI_json_t* jsonIndex, size_t* fieldCount, TRI_idx_iid_t iid) {
  TRI_json_t* keyValues;
  size_t j;

  keyValues = TRI_LookupArrayJson(jsonIndex, "fields");

  if (keyValues == NULL || keyValues->_type != TRI_JSON_LIST) {
    LOG_ERROR("ignoring index %lu, 'fields' must be a list", (unsigned long) iid);

    TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    return NULL;
  }

  
  *fieldCount = keyValues->_value._objects._length;

  
  // ...........................................................................
  // Some simple checks
  // ...........................................................................
  
  for (j = 0;  j < *fieldCount;  ++j) {
    TRI_json_t* keyValue;
    TRI_json_t* key;
    TRI_json_t* value;
 

    // .........................................................................   
    // Extract the jth key value pair
    // .........................................................................   
  
    keyValue = TRI_AtVector(&keyValues->_value._objects, j);
  
  
    // .........................................................................   
    // The length of this key value pair must be two
    // .........................................................................   
    
    if (keyValue == NULL  || keyValue->_value._objects._length != 2) {
      LOG_ERROR("ignoring index %lu, 'fields' must be a list of key value pairs", (unsigned long) iid);
      TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
      return NULL;
    }    

    
    // .........................................................................   
    // Extract the key
    // .........................................................................   
  
    key = TRI_AtVector(&keyValue->_value._objects, 0);
    
    if (key == NULL || key->_type != TRI_JSON_STRING) {
      LOG_ERROR("ignoring index %lu, key in 'fields' pair must be an attribute (string)", (unsigned long) iid);
      TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
      return NULL;
    }
    
    
    // .........................................................................   
    // Extract the value
    // .........................................................................   

    value = TRI_AtVector(&keyValue->_value._objects, 1);
    
    if (value == NULL || value->_type != TRI_JSON_LIST) {
      LOG_ERROR("ignoring index %lu, value in 'fields' pair must be a list ([...])", (unsigned long) iid);
      TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
      return NULL;
    }
    
  }

  return keyValues;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new entry in the immediate indexes
////////////////////////////////////////////////////////////////////////////////

static int CreateImmediateIndexes (TRI_document_collection_t* document,
                                   TRI_doc_mptr_t* header) {
  TRI_df_marker_t const* marker;
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t* found;
  size_t n;
  size_t i;
  int result;
  bool constraint;

  primary = &document->base;
  
  // return in case of a deleted document
  if (header->_validTo != 0) {
    // TODO: fix for trx
    return TRI_ERROR_NO_ERROR;
  }

  // .............................................................................
  // update primary index
  // .............................................................................

  // add a new header
  // found = TRI_InsertKeyAssociativePointer(&primary->_primaryIndex, &header->_did, header, false);
  found = TRI_InsertKeyAssociativePointer(&primary->_primaryIndex, header->_key, header, false);

  // TODO: if TRI_InsertKeyAssociativePointer fails with OOM, it returns NULL. 
  // in case the call succeeds but does not find any previous value, it also returns NULL
  // this function here will continue happily in both cases.
  // These two cases must be distinguishable in order to notify the caller about an error

  if (found != NULL) {
    LOG_ERROR("document '%s' already existed with revision %lu while creating revision %lu",
              header->_key,
              (unsigned long) found->_rid,
              (unsigned long) header->_rid);

    document->_headers->release(document->_headers, header);
    return TRI_set_errno(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);
  }

  // check the document type
  marker = header->_data;
  
  // .............................................................................
  // update edges index
  // .............................................................................

  if (marker->_type == TRI_DOC_MARKER_KEY_EDGE) {
    if (primary->base._type != TRI_COL_TYPE_EDGE) {
      // operation is only permitted for edge collections
      return TRI_set_errno(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
    }

    result = TRI_InsertEdgeDocumentCollection(document, header);
    if (result != TRI_ERROR_NO_ERROR) {
      return result;
    }
  }

  // .............................................................................
  // update all the other indices
  // .............................................................................

  n = document->_allIndexes._length;
  result = TRI_ERROR_NO_ERROR;
  constraint = false;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;
    int res;

    idx = document->_allIndexes._buffer[i];
    res = idx->insert(idx, header);

    // in case of no-memory, return immediately
    if (res == TRI_ERROR_OUT_OF_MEMORY) {
      // TODO: do we have to cleanup?
      return res;
    }

    // "prefer" unique constraint violated
    if (res == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED) {
      constraint = true;
    }
    else if (res != TRI_ERROR_NO_ERROR) {
      result = res;
    }
  }

  if (constraint) {
    return TRI_set_errno(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);
  }

  if (result != TRI_ERROR_NO_ERROR) {
    return TRI_set_errno(result);
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates the immediate indexes
////////////////////////////////////////////////////////////////////////////////

static int UpdateImmediateIndexes (TRI_document_collection_t* collection,
                                   TRI_doc_mptr_t const* header,
                                   TRI_doc_mptr_t const* update) {

  union { TRI_doc_mptr_t const* c; TRI_doc_mptr_t* v; } change;
  TRI_shaped_json_t old;
  bool constraint;
  int result;
  size_t i;
  size_t n;

  // get the old document
  TRI_EXTRACT_SHAPED_JSON_MARKER(old, header->_data);

  // .............................................................................
  // update primary index
  // .............................................................................

  // update all fields, the document identifier stays the same
  change.c = header;

  change.v->_rid = update->_rid;
  change.v->_fid = update->_fid;
  change.v->_validFrom = update->_validFrom; 
  change.v->_validTo = update->_validTo; // TODO: fix for trx

  change.v->_data = update->_data;

  // .............................................................................
  // update all the other indices
  // .............................................................................

  n = collection->_allIndexes._length;
  result = TRI_ERROR_NO_ERROR;
  constraint = false;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;
    int res;

    idx = collection->_allIndexes._buffer[i];
    res = idx->update(idx, header, &old);

    // in case of no-memory, return immediately
    if (res == TRI_ERROR_OUT_OF_MEMORY) {
      return res;
    }

    // "prefer" unique constraint violated
    if (res == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED) {
      constraint = true;
    }
    else if (res != TRI_ERROR_NO_ERROR) {
      result = res;
    }
  }

  if (constraint) {
    return TRI_set_errno(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an entry from the immediate indexes
////////////////////////////////////////////////////////////////////////////////

static int DeleteImmediateIndexes (TRI_document_collection_t* collection,
                                   TRI_doc_mptr_t const* header,
                                   TRI_voc_tick_t deletion) {
  union { TRI_doc_mptr_t const* c; TRI_doc_mptr_t* v; } change;
  TRI_primary_collection_t* primary;
  TRI_df_marker_t const* marker;
  TRI_doc_mptr_t* found;
  size_t n;
  size_t i;
  int result;

  // set the deletion flag
  change.c = header;
  change.v->_validFrom = deletion;
  change.v->_validTo   = deletion; // TODO: fix for trx

  primary = &collection->base;

  // .............................................................................
  // remove from main index
  // .............................................................................

  found = TRI_RemoveKeyAssociativePointer(&primary->_primaryIndex, header->_key);

  if (found == NULL) {
    return TRI_set_errno(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
  }

  // check the document type
  marker = header->_data;
  
  // .............................................................................
  // update edges index
  // .............................................................................
  
  // delete edges
  if (marker->_type == TRI_DOC_MARKER_KEY_EDGE) {
    if (collection->base.base._type != TRI_COL_TYPE_EDGE) {
      // operation is only permitted for edge collections
      return TRI_set_errno(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
    }

    TRI_DeleteEdgeDocumentCollection(collection, header);
  }

  // .............................................................................
  // remove from all other indexes
  // .............................................................................

  n = collection->_allIndexes._length;
  result = TRI_ERROR_NO_ERROR;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;
    int res;

    idx = collection->_allIndexes._buffer[i];
    res = idx->remove(idx, header);

    if (res != TRI_ERROR_NO_ERROR) {
      result = res;
    }
  }

  // and release the header pointer
  collection->_headers->release(collection->_headers, change.v);

  // that's it
  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises an index with all existing documents
////////////////////////////////////////////////////////////////////////////////

static int FillIndex (TRI_document_collection_t* document, TRI_index_t* idx) {
  TRI_doc_mptr_t const* mptr;
  TRI_primary_collection_t* primary;
  TRI_doc_operation_context_t context;
  size_t n;
  size_t inserted;
  void** end;
  void** ptr;
  int res;

  primary = &document->base;
  
  TRI_InitContextPrimaryCollection(&context, primary, TRI_DOC_UPDATE_LAST_WRITE, false);

  // update index
  n = primary->_primaryIndex._nrUsed;
  ptr = primary->_primaryIndex._table;
  end = ptr + primary->_primaryIndex._nrAlloc;

  inserted = 0;

  for (;  ptr < end;  ++ptr) {
    if (IsVisible(*ptr, &context)) {
      mptr = *ptr;

      res = idx->insert(idx, mptr);

      if (res != TRI_ERROR_NO_ERROR) {
        LOG_WARNING("failed to insert document '%lu/%s' for index '%lu'",
                    (unsigned long) primary->base._cid,
                    (char*) mptr->_key,
                    (unsigned long) idx->_iid);

        return res;
      }

      ++inserted;

      if (inserted % 10000 == 0) {
        LOG_DEBUG("indexed %ld documents of collection %lu", inserted, (unsigned long) primary->base._cid);
      }
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a path based, unique or non-unique index
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* LookupPathIndexDocumentCollection (TRI_document_collection_t* collection,
                                                       TRI_vector_t const* paths,
                                                       TRI_idx_type_e type,
                                                       bool unique) {
  TRI_index_t* matchedIndex = NULL;                                                                                        
  TRI_vector_t* indexPaths;
  size_t j;
  size_t k;

  // ...........................................................................
  // go through every index and see if we have a match 
  // ...........................................................................
  
  for (j = 0;  j < collection->_allIndexes._length;  ++j) {
    TRI_index_t* idx = collection->_allIndexes._buffer[j];
    bool found       = true;

    // .........................................................................
    // check if the type of the index matches 
    // .........................................................................
    
    if (idx->_type != type) {
      continue;
    }
    

    // .........................................................................
    // check if uniqueness matches
    // .........................................................................
    
    if (idx->_unique != unique) {
      continue;
    }
    
    
    // .........................................................................
    // Now perform checks which are specific to the type of index
    // .........................................................................
        
    switch (type) {
    
      case TRI_IDX_TYPE_BITARRAY_INDEX: {
        TRI_bitarray_index_t* baIndex = (TRI_bitarray_index_t*) idx;
        indexPaths = &(baIndex->_paths);
        break;
      }
      
      case TRI_IDX_TYPE_HASH_INDEX: {
        TRI_hash_index_t* hashIndex = (TRI_hash_index_t*) idx;
        indexPaths = &(hashIndex->_paths);
        break;
      }
      
      case TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX: {
        TRI_priorityqueue_index_t* pqIndex = (TRI_priorityqueue_index_t*) idx;
        indexPaths = &(pqIndex->_paths);
        break;
      }
      
      case TRI_IDX_TYPE_SKIPLIST_INDEX: {
        TRI_skiplist_index_t* slIndex = (TRI_skiplist_index_t*) idx;
        indexPaths = &(slIndex->_paths);
        break;
      }
      
      default: {
        assert(false);
        break;
      }
      
    }
    
    
    // .........................................................................
    // check that the number of paths (fields) in the index matches that
    // of the number of attributes
    // .........................................................................
    
    if (paths->_length != indexPaths->_length) {
      continue;
    }
          
        
    // .........................................................................
    // go through all the attributes and see if they match
    // .........................................................................
    
    for (k = 0;  k < paths->_length;  ++k) {
      TRI_shape_pid_t indexShape = *((TRI_shape_pid_t*)(TRI_AtVector(indexPaths, k)));
      TRI_shape_pid_t givenShape = *((TRI_shape_pid_t*)(TRI_AtVector(paths, k)));

      if (indexShape != givenShape) {
        found = false;
        break;          
      } 
    }  

    // stop if we found a match
    if (found) {
      matchedIndex = idx;
      break;
    }    
  }

  return matchedIndex;  
}



////////////////////////////////////////////////////////////////////////////////
/// @brief restores a bitarray based index (template)
////////////////////////////////////////////////////////////////////////////////

static int BitarrayBasedIndexFromJson (TRI_document_collection_t* document,
                                       TRI_json_t* definition,
                                       TRI_idx_iid_t iid,
                                       TRI_index_t* (*creator)(TRI_document_collection_t*,
                                                               const TRI_vector_pointer_t*,
                                                               const TRI_vector_pointer_t*,
                                                               TRI_idx_iid_t,
                                                               bool,
                                                               bool*, int*, char**)) {
  TRI_index_t* idx;
  TRI_json_t* uniqueIndex;
  TRI_json_t* supportUndefIndex;
  TRI_json_t* keyValues;
  TRI_vector_pointer_t attributes;
  TRI_vector_pointer_t values;
  // bool unique;
  bool supportUndef;
  bool created;
  size_t fieldCount;
  size_t j;
  int errorNum;
  char* errorStr;

  // ...........................................................................
  // extract fields list (which is a list of key/value pairs for a bitarray index
  // ...........................................................................
  
  keyValues = ExtractFieldValues(definition, &fieldCount, iid);
  if (keyValues == NULL) {
    return TRI_errno();
  }

  
  // ...........................................................................
  // For a bitarray index we require at least one attribute path and one set of
  // possible values for that attribute (that is, we require at least one pair)
  // ...........................................................................
  
  if (fieldCount < 1) {
    LOG_ERROR("ignoring index %lu, need at least one attribute path and one list of values",(unsigned long) iid);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  
  // ...........................................................................
  // A bitarray index is always (for now) non-unique. Irrespective of this fact
  // attempt to extract the 'uniqueness value' from the json object representing
  // the bitarray index.
  // ...........................................................................
  
  // unique = false;
  uniqueIndex = TRI_LookupArrayJson(definition, "unique");
  if (uniqueIndex == NULL || uniqueIndex->_type != TRI_JSON_BOOLEAN) {
    LOG_ERROR("ignoring index %lu, could not determine if unique or non-unique", (unsigned long) iid);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }  
  // unique = uniqueIndex->_value._boolean;
   

  // ...........................................................................
  // A bitarray index can support documents where one or more attributes are 
  // undefined. Determine if this is the case.
  // ...........................................................................
  
  supportUndef = false;
  supportUndefIndex = TRI_LookupArrayJson(definition, "undefined");
  if (supportUndefIndex == NULL || supportUndefIndex->_type != TRI_JSON_BOOLEAN) {
    LOG_ERROR("ignoring index %lu, could not determine if index supports undefined values", (unsigned long) iid);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }  
  supportUndef = supportUndefIndex->_value._boolean;
   
  // ...........................................................................
  // Initialise the vectors in which we store the fields and their corresponding
  // values
  // ...........................................................................
  
  TRI_InitVectorPointer(&attributes, TRI_CORE_MEM_ZONE);
  TRI_InitVectorPointer(&values, TRI_CORE_MEM_ZONE);

  
  // ...........................................................................
  // find fields and values and store them in the vector pointers
  // ...........................................................................
  
  for (j = 0;  j < fieldCount;  ++j) {
    TRI_json_t* keyValue;
    TRI_json_t* key;
    TRI_json_t* value;
    
    keyValue = TRI_AtVector(&keyValues->_value._objects, j);
    key      = TRI_AtVector(&keyValue->_value._objects, 0);
    value    = TRI_AtVector(&keyValue->_value._objects, 1);

    TRI_PushBackVectorPointer(&attributes, key->_value._string.data);
    TRI_PushBackVectorPointer(&values, value);
  }  

  
  // ...........................................................................
  // attempt to create the index or retrieve an existing one
  // ...........................................................................
  errorStr = NULL;
  idx = creator(document, &attributes, &values, iid, supportUndef, &created, &errorNum, &errorStr);


  // ...........................................................................
  // cleanup
  // ...........................................................................
  
  TRI_DestroyVectorPointer(&attributes);
  TRI_DestroyVectorPointer(&values);
  

  // ...........................................................................
  // Check if the creation or lookup succeeded
  // ...........................................................................
  
  if (idx == NULL) {
    LOG_ERROR("cannot create bitarray index %lu", (unsigned long) iid);
    if (errorStr != NULL) {
      LOG_TRACE(errorStr);
      TRI_Free(TRI_CORE_MEM_ZONE, errorStr);  
    }  
    return errorNum;
  }

  return TRI_ERROR_NO_ERROR;
}


////////////////////////////////////////////////////////////////////////////////
/// @brief restores a path based index (template)
////////////////////////////////////////////////////////////////////////////////

static int PathBasedIndexFromJson (TRI_document_collection_t* document,
                                   TRI_json_t* definition,
                                   TRI_idx_iid_t iid,
                                   TRI_index_t* (*creator)(TRI_document_collection_t*,
                                                           TRI_vector_pointer_t const*,
                                                           TRI_idx_iid_t,
                                                           bool,
                                                           bool*)) {
  TRI_index_t* idx;
  TRI_json_t* bv;
  TRI_json_t* fld;
  TRI_json_t* fieldStr;
  TRI_vector_pointer_t attributes;
  bool unique;
  size_t fieldCount;
  size_t j;
  
  // extract fields
  fld = ExtractFields(definition, &fieldCount, iid);

  if (fld == NULL) {
    return TRI_errno();
  }

  // extract the list of fields
  if (fieldCount < 1) {
    LOG_ERROR("ignoring index %lu, need at least von attribute path",(unsigned long) iid);

    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  // determine if the hash index is unique or non-unique
  unique = false;
  bv = TRI_LookupArrayJson(definition, "unique");

  if (bv != NULL && bv->_type == TRI_JSON_BOOLEAN) {
    unique = bv->_value._boolean;
  }
  else {
    LOG_ERROR("ignoring index %lu, could not determine if unique or non-unique", (unsigned long) iid);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }  
    
  // Initialise the vector in which we store the fields on which the hashing
  // will be based.
  TRI_InitVectorPointer(&attributes, TRI_CORE_MEM_ZONE);
    
  // find fields
  for (j = 0;  j < fieldCount;  ++j) {
    fieldStr = TRI_AtVector(&fld->_value._objects, j);

    TRI_PushBackVectorPointer(&attributes, fieldStr->_value._string.data);
  }  

  // create the index
  idx = creator(document, &attributes, iid, unique, NULL);

  // cleanup
  TRI_DestroyVectorPointer(&attributes);

  if (idx == NULL) {
    LOG_ERROR("cannot create hash index %lu", (unsigned long) iid);
    return TRI_errno();
  }

  return TRI_ERROR_NO_ERROR;
}



////////////////////////////////////////////////////////////////////////////////
/// @brief compares pid and name
////////////////////////////////////////////////////////////////////////////////

static int ComparePidName (void const* left, void const* right) {
  pid_name_t const* l = left;
  pid_name_t const* r = right;

  return l->_pid - r->_pid;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a description of all indexes
////////////////////////////////////////////////////////////////////////////////

TRI_vector_pointer_t* TRI_IndexesDocumentCollection (TRI_document_collection_t* document,
                                                     const bool lock) {
  TRI_vector_pointer_t* vector;
  TRI_primary_collection_t* primary;
  size_t n;
  size_t i;

  vector = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_vector_pointer_t), false);
  if (!vector) {
    return NULL;
  }

  primary = &document->base;
  
  TRI_InitVectorPointer(vector, TRI_UNKNOWN_MEM_ZONE);

  // .............................................................................
  // inside read-lock
  // .............................................................................

  if (lock) {
    TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  }

  n = document->_allIndexes._length;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;
    TRI_json_t* json;

    idx = document->_allIndexes._buffer[i];

    json = idx->json(idx, primary);

    if (json != NULL) {
      TRI_PushBackVectorPointer(vector, json);
    }
  }

  if (lock) {
    TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  }

  // .............................................................................
  // outside read-lock
  // .............................................................................

  return vector;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drops an index
////////////////////////////////////////////////////////////////////////////////

bool TRI_DropIndexDocumentCollection (TRI_document_collection_t* document, TRI_idx_iid_t iid) {
  TRI_index_t* found;
  TRI_primary_collection_t* primary;
  bool removeResult;
  size_t n;
  size_t i;

  if (iid == 0) {
    return true;
  }

  found = NULL;
  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  n = document->_allIndexes._length;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;

    idx = document->_allIndexes._buffer[i];

    if (idx->_iid == iid) {
      found = TRI_RemoveVectorPointer(&document->_allIndexes, i);

      if (found != NULL) {
        found->removeIndex(found, primary);
      }

      break;
    }
  }

  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................

  if (found != NULL) {
    removeResult = TRI_RemoveIndexFile(primary, found);
    TRI_FreeIndex(found);
    return removeResult;
  }
  
  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts attribute names to lists of pids and names
///
/// In case of an error, all allocated memory in pids and names will be
/// freed.
////////////////////////////////////////////////////////////////////////////////

int TRI_PidNamesByAttributeNames (TRI_vector_pointer_t const* attributes,
                                  TRI_shaper_t* shaper,
                                  TRI_vector_t* pids,
                                  TRI_vector_pointer_t* names,
                                  bool sorted) {
  pid_name_t* pidnames;
  size_t j;

  // .............................................................................
  // sorted case
  // .............................................................................

  if (sorted) {

    // combine name and pid
    pidnames = TRI_Allocate(TRI_CORE_MEM_ZONE, sizeof(pid_name_t) * attributes->_length, false);
    if (pidnames == NULL) {
      LOG_ERROR("out of memory in TRI_PidNamesByAttributeNames");
      return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
    }
    
    for (j = 0;  j < attributes->_length;  ++j) {
      pidnames[j]._name = attributes->_buffer[j];
      pidnames[j]._pid = shaper->findAttributePathByName(shaper, pidnames[j]._name);   
      
      if (pidnames[j]._pid == 0) {
        TRI_Free(TRI_CORE_MEM_ZONE, pidnames);
        
        return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
      }
    }
    
    // sort according to pid
    qsort(pidnames, attributes->_length, sizeof(pid_name_t), ComparePidName);
    
    // split again
    TRI_InitVector(pids, TRI_CORE_MEM_ZONE, sizeof(TRI_shape_pid_t));
    TRI_InitVectorPointer(names, TRI_CORE_MEM_ZONE);
    
    for (j = 0;  j < attributes->_length;  ++j) {
      TRI_PushBackVector(pids, &pidnames[j]._pid);
      TRI_PushBackVectorPointer(names, pidnames[j]._name);
    }

    TRI_Free(TRI_CORE_MEM_ZONE, pidnames);
  }

  // .............................................................................
  // unsorted case
  // .............................................................................

  else {
    TRI_InitVector(pids, TRI_CORE_MEM_ZONE, sizeof(TRI_shape_pid_t));
    TRI_InitVectorPointer(names, TRI_CORE_MEM_ZONE);
    
    for (j = 0;  j < attributes->_length;  ++j) {
      char* name;
      TRI_shape_pid_t pid;

      name = attributes->_buffer[j];
      pid = shaper->findAttributePathByName(shaper, name);

      if (pid == 0) {
        TRI_DestroyVector(pids);
        TRI_DestroyVectorPointer(names);

        return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
      }

      TRI_PushBackVector(pids, &pid);
      TRI_PushBackVectorPointer(names, name);
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    CAP CONSTRAINT
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a cap constraint to a collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreateCapConstraintDocumentCollection (TRI_document_collection_t* document,
                                                           size_t size,
                                                           TRI_idx_iid_t iid,
                                                           bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;

  primary = &document->base;

  if (created != NULL) {
    *created = false;
  }

  // check if we already know a cap constraint
  if (primary->_capConstraint != NULL) {
    if (primary->_capConstraint->_size == size) {
      return &primary->_capConstraint->base;
    }
    else {
      TRI_set_errno(TRI_ERROR_ARANGO_CAP_CONSTRAINT_ALREADY_DEFINED);
      return NULL;
    }
  }

  // create a new index
  idx = TRI_CreateCapConstraint(primary, size);
  if (idx == NULL) {
    TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);

    return NULL;
  }

  if (iid) {
    idx->_iid = iid;
  }

  // initialises the index with all existing documents
  res = FillIndex(document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeCapConstraint(idx);
    return NULL;
  }
  
  // and store index
  TRI_PushBackVectorPointer(&document->_allIndexes, idx);
  primary->_capConstraint = (TRI_cap_constraint_t*) idx;

  if (created != NULL) {
    *created = true;
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int CapConstraintFromJson (TRI_document_collection_t* document,
                                  TRI_json_t* definition,
                                  TRI_idx_iid_t iid) {
  TRI_json_t* num;
  TRI_index_t* idx;
  size_t size;

  num = TRI_LookupArrayJson(definition, "size");

  if (num == NULL || num->_type != TRI_JSON_NUMBER) {
    LOG_ERROR("ignoring cap constraint %lu, 'size' missing", (unsigned long) iid);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  if (num->_value._number < 1.0) {
    LOG_ERROR("ignoring cap constraint %lu, 'size' %f must be at least 1", (unsigned long) iid, num->_value._number);
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  size = (size_t) num->_value._number; 

  idx = CreateCapConstraintDocumentCollection(document, size, iid, NULL);

  return idx == NULL ? TRI_errno() : TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a cap constraint exists
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureCapConstraintDocumentCollection (TRI_document_collection_t* document,
                                                        size_t size,
                                                        bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;

  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  idx = CreateCapConstraintDocumentCollection(document, size, 0, created);
  
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);

    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                         GEO INDEX
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a geo index to a collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreateGeoIndexDocumentCollection (TRI_document_collection_t* document,
                                                      char const* location,
                                                      char const* latitude,
                                                      char const* longitude,
                                                      bool geoJson,
                                                      bool constraint,
                                                      bool ignoreNull,
                                                      TRI_idx_iid_t iid,
                                                      bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  TRI_shape_pid_t lat;
  TRI_shape_pid_t loc;
  TRI_shape_pid_t lon;
  TRI_shaper_t* shaper;
  int res;

  lat = 0;
  lon = 0;
  loc = 0;
  idx = NULL;

  primary = &document->base;
  shaper = primary->_shaper;

  if (location != NULL) {
    loc = shaper->findAttributePathByName(shaper, location);

    if (loc == 0) {
      TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
      return NULL;
    }
  }

  if (latitude != NULL) {
    lat = shaper->findAttributePathByName(shaper, latitude);

    if (lat == 0) {
      TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
      return NULL;
    }
  }

  if (longitude != NULL) {
    lon = shaper->findAttributePathByName(shaper, longitude);

    if (lon == 0) {
      TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
      return NULL;
    }
  }

  // check, if we know the index
  if (location != NULL) {
    idx = TRI_LookupGeoIndex1DocumentCollection(document, loc, geoJson, constraint, ignoreNull);
  }
  else if (longitude != NULL && latitude != NULL) {
    idx = TRI_LookupGeoIndex2DocumentCollection(document, lat, lon, constraint, ignoreNull);
  }
  else {
    TRI_set_errno(TRI_ERROR_INTERNAL);
    LOG_TRACE("expecting either 'location' or 'latitude' and 'longitude'");
    return NULL;
  }

  if (idx != NULL) {
    LOG_TRACE("geo-index already created for location '%s'", location);

    if (created != NULL) {
      *created = false;
    }

    return idx;
  }

  // create a new index
  if (location != NULL) {
    idx = TRI_CreateGeo1Index(primary, location, loc, geoJson, constraint, ignoreNull);

    LOG_TRACE("created geo-index for location '%s': %d",
              location,
              (unsigned long) loc);
  }
  else if (longitude != NULL && latitude != NULL) {
    idx = TRI_CreateGeo2Index(primary, latitude, lat, longitude, lon, constraint, ignoreNull);

    LOG_TRACE("created geo-index for location '%s': %d, %d",
              location,
              (unsigned long) lat,
              (unsigned long) lon);
  }

  if (iid) {
    idx->_iid = iid;
  }

  // initialises the index with all existing documents
  res = FillIndex(document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeGeoIndex(idx);
    return NULL;
  }
  
  // and store index
  TRI_PushBackVectorPointer(&document->_allIndexes, idx);

  if (created != NULL) {
    *created = true;
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int GeoIndexFromJson (TRI_document_collection_t* document,
                             TRI_json_t* definition,
                             TRI_idx_iid_t iid) {
  TRI_index_t* idx;
  TRI_json_t* bv;
  TRI_json_t* fld;
  bool constraint;
  bool ignoreNull;
  char const* typeStr;
  size_t fieldCount;

  typeStr = TRI_LookupArrayJson(definition, "type")->_value._string.data;

  // extract fields
  fld = ExtractFields(definition, &fieldCount, iid);

  if (fld == NULL) {
    return TRI_errno();
  }

  // extract constraint
  constraint = false;
  bv = TRI_LookupArrayJson(definition, "constraint");
    
  if (bv != NULL && bv->_type == TRI_JSON_BOOLEAN) {
    constraint = bv->_value._boolean;
  }

  // extract ignore null
  ignoreNull = false;
  bv = TRI_LookupArrayJson(definition, "ignoreNull");
  
  if (bv != NULL && bv->_type == TRI_JSON_BOOLEAN) {
    ignoreNull = bv->_value._boolean;
  }

  // list style
  if (TRI_EqualString(typeStr, "geo1")) {
    bool geoJson;

    // extract geo json
    geoJson = false;
    bv = TRI_LookupArrayJson(definition, "geoJson");
    
    if (bv != NULL && bv->_type == TRI_JSON_BOOLEAN) {
      geoJson = bv->_value._boolean;
    }

    // need just one field
    if (fieldCount == 1) {
      TRI_json_t* loc;

      loc = TRI_AtVector(&fld->_value._objects, 0);

      idx = CreateGeoIndexDocumentCollection(document,
                                        loc->_value._string.data,
                                        NULL, 
                                        NULL, 
                                        geoJson,
                                        constraint,
                                        ignoreNull,
                                        iid,
                                        NULL);

      return idx == NULL ? TRI_errno() : TRI_ERROR_NO_ERROR;
    }
    else {
      LOG_ERROR("ignoring %s-index %lu, 'fields' must be a list with 1 entries",
                typeStr, (unsigned long) iid);
        
      return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    }
  }

  // attribute style
  else if (TRI_EqualString(typeStr, "geo2")) {
    if (fieldCount == 2) {
      TRI_json_t* lat;
      TRI_json_t* lon;

      lat = TRI_AtVector(&fld->_value._objects, 0);
      lon = TRI_AtVector(&fld->_value._objects, 1);

      idx = CreateGeoIndexDocumentCollection(document,
                                        NULL,
                                        lat->_value._string.data,
                                        lon->_value._string.data,
                                        false,
                                        constraint,
                                        ignoreNull, 
                                        iid, 
                                        NULL);

      return idx == NULL ? TRI_errno() : TRI_ERROR_NO_ERROR;
    }
    else {
      LOG_ERROR("ignoring %s-index %lu, 'fields' must be a list with 2 entries",
                typeStr, (unsigned long) iid);

      return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    }
  }

  else {
    assert(false);
  }

  return 0; // shut the vc++ up
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a geo index, list style
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupGeoIndex1DocumentCollection (TRI_document_collection_t* document,
                                                    TRI_shape_pid_t location,
                                                    bool geoJson,
                                                    bool constraint,
                                                    bool ignoreNull) {
  size_t n;
  size_t i;

  n = document->_allIndexes._length;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;

    idx = document->_allIndexes._buffer[i];

    if (idx->_type == TRI_IDX_TYPE_GEO1_INDEX) {
      TRI_geo_index_t* geo = (TRI_geo_index_t*) idx;

      if (geo->_location != 0 && geo->_location == location && geo->_geoJson == geoJson && geo->_constraint == constraint) {
        if (! constraint || geo->base._ignoreNull == ignoreNull) {
          return idx;
        }
      }
    }
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a geo index, attribute style
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupGeoIndex2DocumentCollection (TRI_document_collection_t* document,
                                                    TRI_shape_pid_t latitude,
                                                    TRI_shape_pid_t longitude,
                                                    bool constraint,
                                                    bool ignoreNull) {
  size_t n;
  size_t i;

  n = document->_allIndexes._length;

  for (i = 0;  i < n;  ++i) {
    TRI_index_t* idx;

    idx = document->_allIndexes._buffer[i];

    if (idx->_type == TRI_IDX_TYPE_GEO2_INDEX) {
      TRI_geo_index_t* geo = (TRI_geo_index_t*) idx;

      if (geo->_latitude != 0 && geo->_longitude != 0 && geo->_latitude == latitude && geo->_longitude == longitude && geo->_constraint == constraint) {
        if (! constraint || geo->base._ignoreNull == ignoreNull) {
          return idx;
        }
      }
    }
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a geo index exists, list style
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureGeoIndex1DocumentCollection (TRI_document_collection_t* document,
                                                    char const* location,
                                                    bool geoJson,
                                                    bool constraint,
                                                    bool ignoreNull,
                                                    bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;

  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  idx = CreateGeoIndexDocumentCollection(document, location, NULL, NULL, geoJson, constraint, ignoreNull, 0, created);
    
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);

    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a geo index exists, attribute style
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureGeoIndex2DocumentCollection (TRI_document_collection_t* document,
                                                    char const* latitude,
                                                    char const* longitude,
                                                    bool constraint,
                                                    bool ignoreNull,
                                                    bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;

  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  idx = CreateGeoIndexDocumentCollection(document, NULL, latitude, longitude, false, constraint, ignoreNull, 0, created);
    
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);

    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  
  return TRI_ERROR_NO_ERROR;
}


////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                        HASH INDEX
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a hash index to the collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreateHashIndexDocumentCollection (TRI_document_collection_t* document,
                                                       TRI_vector_pointer_t const* attributes,
                                                       TRI_idx_iid_t iid,
                                                       bool unique,
                                                       bool* created) {
  TRI_index_t* idx;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int res;

  idx = NULL;
  
  // determine the sorted shape ids for the attributes
  res = TRI_PidNamesByAttributeNames(attributes, 
                                     document->base._shaper,
                                     &paths,
                                     &fields,
                                     true);

  if (res != TRI_ERROR_NO_ERROR) {
    if (created != NULL) {
      *created = false;
    }

    return NULL;
  }

  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_HASH_INDEX, unique);
  
  if (idx != NULL) {
    TRI_DestroyVector(&paths);
    TRI_DestroyVectorPointer(&fields);
    LOG_TRACE("hash-index already created");

    if (created != NULL) {
      *created = false;
    }

    return idx;
  }

  // create the hash index. we'll provide it with the current number of documents
  // in the collection so the index can do a sensible memory preallocation
  idx = TRI_CreateHashIndex(&document->base, 
                            &fields, 
                            &paths, 
                            unique, 
                            document->base._primaryIndex._nrUsed);

  // release memory allocated to vector
  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);
  
  // if index id given, use it otherwise use the default.
  if (iid) {
    idx->_iid = iid;
  }
  
  // initialises the index with all existing documents
  res = FillIndex(document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeHashIndex(idx);
    return NULL;
  }
  
  // store index and return
  TRI_PushBackVectorPointer(&document->_allIndexes, idx);

  if (created != NULL) {
    *created = true;
  }

  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int HashIndexFromJson (TRI_document_collection_t* document,
                              TRI_json_t* definition,
                              TRI_idx_iid_t iid) {
  return PathBasedIndexFromJson(document, definition, iid, CreateHashIndexDocumentCollection);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a hash index (unique or non-unique)
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupHashIndexDocumentCollection (TRI_document_collection_t* document,
                                                    TRI_vector_pointer_t const* attributes,
                                                    bool unique) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int res;

  primary = &document->base;

  // determine the sorted shape ids for the attributes
  res = TRI_PidNamesByAttributeNames(attributes, 
                                     primary->_shaper,
                                     &paths,
                                     &fields,
                                     true);

  if (res != TRI_ERROR_NO_ERROR) {
    return NULL;
  }

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_HASH_INDEX, unique);
  
  TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................

  // release memory allocated to vector
  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a hash index exists
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureHashIndexDocumentCollection (TRI_document_collection_t* document,
                                                    TRI_vector_pointer_t const* attributes,
                                                    bool unique,
                                                    bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;
  
  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // given the list of attributes (as strings) 
  idx = CreateHashIndexDocumentCollection(document, attributes, 0, unique, created);
  
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);

    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  
  return TRI_ERROR_NO_ERROR;
}                                                

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    SKIPLIST INDEX
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a skiplist index to the collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreateSkiplistIndexDocumentCollection (TRI_document_collection_t* document,
                                                           TRI_vector_pointer_t const* attributes,
                                                           TRI_idx_iid_t iid,
                                                           bool unique,
                                                           bool* created) {
  TRI_index_t* idx;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int res;

  res = TRI_PidNamesByAttributeNames(attributes, 
                                     document->base._shaper,
                                     &paths,
                                     &fields,
                                     false);

  if (res != TRI_ERROR_NO_ERROR) {
    if (created != NULL) {
      *created = false;
    }

    return NULL;
  }

  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_SKIPLIST_INDEX, unique);
  
  if (idx != NULL) {
    TRI_DestroyVector(&paths);
    TRI_DestroyVectorPointer(&fields);
    LOG_TRACE("skiplist-index already created");

    if (created != NULL) {
      *created = false;
    }

    return idx;
  }

  // Create the skiplist index
  idx = TRI_CreateSkiplistIndex(&document->base, &fields, &paths, unique);

  // release memory allocated to vector
  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);
  
  // If index id given, use it otherwise use the default.
  if (iid) {
    idx->_iid = iid;
  }
  
  // initialises the index with all existing documents
  res = FillIndex(document, idx);
  
  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeSkiplistIndex(idx);
    return NULL;
  }
  
  // store index and return
  TRI_PushBackVectorPointer(&document->_allIndexes, idx);
  
  if (created != NULL) {
    *created = true;
  }
  
  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int SkiplistIndexFromJson (TRI_document_collection_t* document,
                                  TRI_json_t* definition,
                                  TRI_idx_iid_t iid) {
  return PathBasedIndexFromJson(document, definition, iid, CreateSkiplistIndexDocumentCollection);
}
                                                  
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a skiplist index (unique or non-unique)
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupSkiplistIndexDocumentCollection (TRI_document_collection_t* document,
                                                        TRI_vector_pointer_t const* attributes,
                                                        bool unique) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int res;
  
  primary = &document->base;
  
  // determine the unsorted shape ids for the attributes
  res = TRI_PidNamesByAttributeNames(attributes, 
                                     primary->_shaper,
                                     &paths,
                                     &fields,
                                     false);

  if (res != TRI_ERROR_NO_ERROR) {
    return NULL;
  }

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_SKIPLIST_INDEX, unique);
  
  TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................

  // release memory allocated to vector
  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);

  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a skiplist index exists
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureSkiplistIndexDocumentCollection (TRI_document_collection_t* document,
                                                        TRI_vector_pointer_t const* attributes,
                                                        bool unique,
                                                        bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;
  
  primary = &document->base;

  // .............................................................................
  // inside write-lock the collection
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  idx = CreateSkiplistIndexDocumentCollection(document, attributes, 0, unique, created);
  
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);
  
    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  
  return TRI_ERROR_NO_ERROR;
}                                                
                                                
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                              PRIORITY QUEUE INDEX
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a priroity queue index to the collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreatePriorityQueueIndexDocumentCollection (TRI_document_collection_t* document,
                                                                TRI_vector_pointer_t const* attributes,
                                                                TRI_idx_iid_t iid,
                                                                bool unique,
                                                                bool* created) {
  TRI_index_t* idx     = NULL;
  TRI_shaper_t* shaper = document->base._shaper;
  TRI_vector_t paths;
  TRI_vector_pointer_t fields;
  int res;
  size_t j;


  TRI_InitVector(&paths, TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_shape_pid_t));
  TRI_InitVectorPointer(&fields, TRI_UNKNOWN_MEM_ZONE);

  // ...........................................................................
  // Determine the shape ids for the attributes
  // ...........................................................................

  for (j = 0;  j < attributes->_length;  ++j) {
    char* path = attributes->_buffer[j];
    TRI_shape_pid_t shape = shaper->findAttributePathByName(shaper, path);   

    if (shape == 0) {
      TRI_DestroyVector(&paths);
      TRI_DestroyVectorPointer(&fields);
      return NULL;
    }

    TRI_PushBackVector(&paths, &shape);
    TRI_PushBackVectorPointer(&fields, path);
  }
  
  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  idx = TRI_LookupPriorityQueueIndexDocumentCollection(document, &paths);
  
  if (idx != NULL) {
    TRI_DestroyVector(&paths);
    TRI_DestroyVectorPointer(&fields);

    LOG_TRACE("priority queue  index already created");

    if (created != NULL) {
      *created = false;
    }

    return idx;
  }

  // ...........................................................................
  // Create the priority queue index
  // ...........................................................................

  idx = TRI_CreatePriorityQueueIndex(&document->base, &fields, &paths, unique);

  // ...........................................................................
  // If index id given, use it otherwise use the default.
  // ...........................................................................

  if (iid) {
    idx->_iid = iid;
  }
  
  // ...........................................................................
  // initialises the index with all existing documents
  // ...........................................................................

  res = FillIndex(document, idx);
  
  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreePriorityQueueIndex(idx);
    return NULL;
  }
  
  // ...........................................................................
  // store index
  // ...........................................................................

  TRI_PushBackVectorPointer(&document->_allIndexes, idx);
  
  // ...........................................................................
  // release memory allocated to vector
  // ...........................................................................

  TRI_DestroyVector(&paths);

  if (created != NULL) {
    *created = true;
  }
  
  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int PriorityQueueFromJson (TRI_document_collection_t* document,
                                  TRI_json_t* definition,
                                  TRI_idx_iid_t iid) {
  return PathBasedIndexFromJson(document, definition, iid, CreatePriorityQueueIndexDocumentCollection);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a priority queue index (non-unique)
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupPriorityQueueIndexDocumentCollection (TRI_document_collection_t* document,
                                                             const TRI_vector_t* paths) {
  TRI_index_t* matchedIndex = NULL;                                                                                        
  size_t j, k;
  
  // ........................................................................... 
  // go through every index and see if we have a match 
  // ........................................................................... 
  
  for (j = 0;  j < document->_allIndexes._length;  ++j) {
    TRI_index_t* idx                    = document->_allIndexes._buffer[j];
    TRI_priorityqueue_index_t* pqIndex  = (TRI_priorityqueue_index_t*) idx;
    bool found                          = true;

    // .........................................................................
    // check that the type of the index is in fact a skiplist index 
    // .........................................................................
        
    if (idx->_type != TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX) {
      continue;
    }
        
    // .........................................................................
    // check that the number of paths (fields) in the index matches that
    // of the number of attributes
    // .........................................................................
        
    if (paths->_length != pqIndex->_paths._length) {
      continue;
    }
        
    // .........................................................................
    // Go through all the attributes and see if they match
    // .........................................................................

    for (k = 0; k < paths->_length; ++k) {
      TRI_shape_pid_t field = *((TRI_shape_pid_t*)(TRI_AtVector(&pqIndex->_paths,k)));   
      TRI_shape_pid_t shape = *((TRI_shape_pid_t*)(TRI_AtVector(paths,k)));

      if (field != shape) {
        found = false;
        break;          
      } 
    }  
        

    if (found) {
      matchedIndex = idx;
      break;
    }    
  }
  
  return matchedIndex;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a priority queue index exists
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsurePriorityQueueIndexDocumentCollection(TRI_document_collection_t* document,
                                                            TRI_vector_pointer_t const* attributes,
                                                            bool unique,
                                                            bool* created) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;

  primary = &document->base;

  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // Given the list of attributes (as strings) 
  idx = CreatePriorityQueueIndexDocumentCollection(document, attributes, 0, unique, created);
  
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);

    return res == TRI_ERROR_NO_ERROR ? idx : NULL;
  }
  else {
    return TRI_ERROR_NO_ERROR;
  }
}                                                

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////


// -----------------------------------------------------------------------------
// --SECTION--                                                    BITARRAY INDEX
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a bitarray index to the collection
////////////////////////////////////////////////////////////////////////////////

static TRI_index_t* CreateBitarrayIndexDocumentCollection (TRI_document_collection_t* document,
                                                      const TRI_vector_pointer_t* attributes,
                                                      const TRI_vector_pointer_t* values,
                                                      TRI_idx_iid_t iid,
                                                      bool supportUndef,
                                                      bool* created,
                                                      int* errorNum,
                                                      char** errorStr) {
  TRI_index_t* idx;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int res;

  res = TRI_PidNamesByAttributeNames(attributes, 
                                     document->base._shaper,
                                     &paths,
                                     &fields,
                                     false);

  if (res != TRI_ERROR_NO_ERROR) {
    if (created != NULL) {
      *created = false;
    }
    *errorNum = res;
    *errorStr  = TRI_DuplicateString("Bitarray index attributes could not be accessed.");
    return NULL;
  }

  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_BITARRAY_INDEX, false);
  
  if (idx != NULL) {
  
    // .........................................................................
    // existing index has been located which matches the list of attributes
    // return this one
    // .........................................................................
    
    TRI_DestroyVector(&paths);
    TRI_DestroyVectorPointer(&fields);
    LOG_TRACE("bitarray-index previously created");

    if (created != NULL) {
      *created = false;
    }

    return idx;
  }


  // ...........................................................................
  // Create the bitarray index
  // ...........................................................................
  
  idx = TRI_CreateBitarrayIndex(&document->base, &fields, &paths, (TRI_vector_pointer_t*)(values), supportUndef, errorNum, errorStr);

  
  // ...........................................................................
  // release memory allocated to fields & paths vectors
  // ...........................................................................
  
  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);


  // ...........................................................................
  // Perhaps the index was not created in the function TRI_CreateBitarrayIndex
  // ...........................................................................
  
  if (idx == NULL) {
    LOG_TRACE("bitarray index could not be created in TRI_CreateBitarrayIndex");
    if (created != NULL) {
      *created = false;
    }
    return idx;
  }
  
  // ...........................................................................
  // If an index id given, use it otherwise use the default (generate one)
  // ...........................................................................
  
  if (iid) {
    idx->_iid = iid;
  }
  

  // ...........................................................................
  // initialises the index with all existing documents
  // ...........................................................................
  
  res = FillIndex(document, idx);
  
  if (res != TRI_ERROR_NO_ERROR) {
  
    // .........................................................................
    // for some reason one or more of the existing documents has caused the 
    // index to fail. Remove the index from the collection and return null.
    // .........................................................................
    
    *errorNum = res;
    *errorStr = TRI_DuplicateString("Bitarray index creation aborted due to documents within collection.");
    TRI_FreeBitarrayIndex(idx);
    return NULL;
  }
  

  // ...........................................................................
  // store index within the collection and return
  // ...........................................................................
  
  TRI_PushBackVectorPointer(&document->_allIndexes, idx);
  
  if (created != NULL) {
    *created = true;
  }
  
  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int BitarrayIndexFromJson (TRI_document_collection_t* document,
                                  TRI_json_t* definition,
                                  TRI_idx_iid_t iid) {
  return BitarrayBasedIndexFromJson(document, definition, iid, CreateBitarrayIndexDocumentCollection);
}
                                                  
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a bitarray index
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_LookupBitarrayIndexDocumentCollection (TRI_document_collection_t* document,
                                                        const TRI_vector_pointer_t* attributes) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  TRI_vector_pointer_t fields;
  TRI_vector_t paths;
  int result;
  
  primary = &document->base;
  
  // ...........................................................................
  // determine the unsorted shape ids for the attributes
  // ...........................................................................
  
  result = TRI_PidNamesByAttributeNames(attributes, primary->_shaper, &paths,
                                     &fields, false);

  if (result != TRI_ERROR_NO_ERROR) {
    return NULL;
  }

  
  // .............................................................................
  // inside write-lock
  // .............................................................................

  TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  
  // .............................................................................
  // attempt to go through the indexes within the collection and see if we can
  // locate the index
  // .............................................................................
  
  idx = LookupPathIndexDocumentCollection(document, &paths, TRI_IDX_TYPE_SKIPLIST_INDEX, false);
  
  
  TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);

  // .............................................................................
  // outside write-lock
  // .............................................................................

  // .............................................................................
  // release memory allocated to vector
  // .............................................................................

  TRI_DestroyVector(&paths);
  TRI_DestroyVectorPointer(&fields);

  return idx;  
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a bitarray index exists
////////////////////////////////////////////////////////////////////////////////

TRI_index_t* TRI_EnsureBitarrayIndexDocumentCollection (TRI_document_collection_t* document,
                                                        const TRI_vector_pointer_t* attributes,
                                                        const TRI_vector_pointer_t* values,
                                                        bool supportUndef,
                                                        bool* created,
                                                        int* errorCode,
                                                        char** errorStr) {
  TRI_index_t* idx;
  TRI_primary_collection_t* primary;
  int res;
  
  primary = &document->base;

  *errorCode = TRI_ERROR_NO_ERROR;
  *errorStr  = NULL;
  
  // .............................................................................
  // inside write-lock the collection
  // .............................................................................

  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  idx = CreateBitarrayIndexDocumentCollection(document, attributes, values, 0, supportUndef, created, errorCode, errorStr);
  
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(primary);
  
  // .............................................................................
  // outside write-lock
  // .............................................................................
  
  
  // .............................................................................
  // The index is 'new' so save it
  // .............................................................................

  if (idx == NULL) {
    return NULL;
  }

  if (created) {
    res = TRI_SaveIndex(primary, idx);
    
    // ...........................................................................    
    // If index could not be saved, report the error and return NULL
    // TODO: get TRI_SaveIndex to report the error
    // ...........................................................................    
    
    if (res == TRI_ERROR_NO_ERROR) {
      return idx;
    }
   
    *errorCode = res;
    *errorStr  = TRI_DuplicateString("Bitarray index could not be saved.");
    return NULL;
  }
  
  // .............................................................................
  // Index already exists so simply return it
  // .............................................................................
  
  return idx;
}                                                
                                                
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////


// -----------------------------------------------------------------------------
// --SECTION--                                           SELECT BY EXAMPLE QUERY
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief checks for match of an example
////////////////////////////////////////////////////////////////////////////////

static bool IsExampleMatch (TRI_shaper_t* shaper,
                            TRI_doc_mptr_t const* doc,
                            size_t len, 
                            TRI_shape_pid_t* pids,
                            TRI_shaped_json_t** values) {
  TRI_shaped_json_t document;
  TRI_shaped_json_t* example;
  TRI_shaped_json_t result;
  TRI_shape_t const* shape;
  bool ok;
  size_t i;

  TRI_EXTRACT_SHAPED_JSON_MARKER(document, doc->_data);

  for (i = 0;  i < len;  ++i) {
    example = values[i];

    ok = TRI_ExtractShapedJsonVocShaper(shaper,
                                        &document,
                                        example->_sid,
                                        pids[i],
                                        &result,
                                        &shape);

    if (! ok || shape == NULL) {
      return false;
    }

    if (result._data.length != example->_data.length) {
      // suppress excessive log spam
      // LOG_TRACE("expecting length %lu, got length %lu for path %lu",
      //           (unsigned long) result._data.length,
      //           (unsigned long) example->_data.length,
      //           (unsigned long) pids[i]);

      return false;
    }

    if (memcmp(result._data.data, example->_data.data, example->_data.length) != 0) {
      // suppress excessive log spam
      // LOG_TRACE("data mismatch at path %lu", (unsigned long) pids[i]);
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a select-by-example query
////////////////////////////////////////////////////////////////////////////////

TRI_vector_t TRI_SelectByExample (TRI_doc_operation_context_t* context,
                                  size_t length,
                                  TRI_shape_pid_t* pids,
                                  TRI_shaped_json_t** values) {
  TRI_shaper_t* shaper;
  TRI_primary_collection_t* primary;
  TRI_doc_mptr_t const** ptr;
  TRI_doc_mptr_t const** end;
  TRI_vector_t filtered;

  primary = context->_collection;

  // use filtered to hold copies of the master pointer
  TRI_InitVector(&filtered, TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_doc_mptr_t));

  // do a full scan
  shaper = primary->_shaper;

  ptr = (TRI_doc_mptr_t const**) (primary->_primaryIndex._table);
  end = (TRI_doc_mptr_t const**) ptr + primary->_primaryIndex._nrAlloc;

  for (;  ptr < end;  ++ptr) {
    if (IsVisible(*ptr, context)) {
      if (IsExampleMatch(shaper, *ptr, length, pids, values)) {
        TRI_PushBackVector(&filtered, *ptr);
      }
    }
  }

  return filtered;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

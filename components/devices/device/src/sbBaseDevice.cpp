/* vim: set sw=2 :miv */
/*
//
// BEGIN SONGBIRD GPL
//
// This file is part of the Songbird web player.
//
// Copyright(c) 2005-2009 POTI, Inc.
// http://songbirdnest.com
//
// This file may be licensed under the terms of of the
// GNU General Public License Version 2 (the "GPL").
//
// Software distributed under the License is distributed
// on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either
// express or implied. See the GPL for the specific language
// governing rights and limitations.
//
// You should have received a copy of the GPL along with this
// program. If not, go to http://www.gnu.org/licenses/gpl.html
// or write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// END SONGBIRD GPL
//
*/

#include "sbBaseDevice.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <vector>

#include <prtime.h>

#include <nsIFileURL.h>
#include <nsIPropertyBag2.h>
#include <nsITimer.h>
#include <nsIURI.h>
#include <nsIVariant.h>
#include <nsIPrefService.h>
#include <nsIPrefBranch.h>
#include <nsIProxyObjectManager.h>

#include <nsArrayUtils.h>
#include <nsAutoLock.h>
#include <nsAutoPtr.h>
#include <nsComponentManagerUtils.h>
#include <nsCRT.h>
#include <nsMemory.h>
#include <nsNetUtil.h>
#include <nsServiceManagerUtils.h>
#include <nsThreadUtils.h>
#include <nsIDOMWindow.h>
#include <nsIPromptService.h>
#include <nsISupportsPrimitives.h>

#include <sbIDeviceContent.h>
#include <sbIDeviceCapabilitiesRegistrar.h>
#include <sbIDeviceEvent.h>
#include <sbIDeviceHelper.h>
#include <sbIDeviceManager.h>
#include <sbIDeviceProperties.h>
#include <sbILibrary.h>
#include <sbILibraryDiffingService.h>
#include <sbIMediaItem.h>
#include <sbIMediaList.h>
#include <sbIOrderableMediaList.h>
#include <sbIPrompter.h>
#include <sbITranscodeManager.h>
#include <sbLocalDatabaseCID.h>
#include <sbPrefBranch.h>
#include <sbStringBundle.h>
#include <sbStringUtils.h>
#include <sbVariantUtils.h>

#include "sbDeviceLibrary.h"
#include "sbDeviceStatus.h"
#include "sbDeviceUtils.h"
#include "sbLibraryListenerHelpers.h"
#include "sbLibraryUtils.h"
#include "sbProxyUtils.h"
#include "sbStandardDeviceProperties.h"
#include "sbStandardProperties.h"

/*
 * To log this module, set the following environment variable:
 *   NSPR_LOG_MODULES=sbBaseDevice:5
 */
#ifdef PR_LOGGING
static PRLogModuleInfo* gBaseDeviceLog = nsnull;
#endif

#undef LOG
#define LOG(args) PR_LOG(gBaseDeviceLog, PR_LOG_WARN, args)


#define DEFAULT_COLUMNSPEC_DEVICE_LIBRARY SB_PROPERTY_TRACKNAME " 265 "     \
                                          SB_PROPERTY_DURATION " 43 "       \
                                          SB_PROPERTY_ARTISTNAME " 177 a "  \
                                          SB_PROPERTY_ALBUMNAME " 159 "     \
                                          SB_PROPERTY_GENRE " 53 "          \
                                          SB_PROPERTY_RATING   " 80"        \

#define PREF_DEVICE_PREFERENCES_BRANCH "songbird.device."
#define PREF_DEVICE_LIBRARY_BASE "library."
#define PREF_WARNING "warning."

#define BATCH_TIMEOUT 100 /* number of milliseconds to wait for batching */

NS_IMPL_THREADSAFE_ISUPPORTS0(sbBaseDevice::TransferRequest)

class MediaListListenerAttachingEnumerator : public sbIMediaListEnumerationListener
{
public:
  MediaListListenerAttachingEnumerator(sbBaseDevice* aDevice)
   : mDevice(aDevice)
   {}
  NS_DECL_ISUPPORTS
  NS_DECL_SBIMEDIALISTENUMERATIONLISTENER
private:
  sbBaseDevice* mDevice;
};

NS_IMPL_ISUPPORTS1(MediaListListenerAttachingEnumerator,
                   sbIMediaListEnumerationListener)

NS_IMETHODIMP MediaListListenerAttachingEnumerator::OnEnumerationBegin(sbIMediaList*,
                                                                       PRUint16 *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = sbIMediaListEnumerationListener::CONTINUE;
  return NS_OK;
}

NS_IMETHODIMP MediaListListenerAttachingEnumerator::OnEnumeratedItem(sbIMediaList*,
                                                                     sbIMediaItem* aItem,
                                                                     PRUint16 *_retval)
{
  NS_ENSURE_ARG_POINTER(aItem);
  NS_ENSURE_ARG_POINTER(_retval);
  NS_ENSURE_TRUE(mDevice, NS_ERROR_NOT_INITIALIZED);

  nsresult rv;

  nsCOMPtr<sbIMediaList> list(do_QueryInterface(aItem, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mDevice->ListenToList(list);
  NS_ENSURE_SUCCESS(rv, rv);

  *_retval = sbIMediaListEnumerationListener::CONTINUE;

  return NS_OK;
}

NS_IMETHODIMP MediaListListenerAttachingEnumerator::OnEnumerationEnd(sbIMediaList*,
                                                                     nsresult)
{
  return NS_OK;
}

class ShowMediaListEnumerator : public sbIMediaListEnumerationListener
{
public:
  explicit ShowMediaListEnumerator(PRBool aHideMediaLists);
  NS_DECL_ISUPPORTS
  NS_DECL_SBIMEDIALISTENUMERATIONLISTENER
private:
  PRBool    mHideMediaLists;
  nsString  mHideMediaListsStringValue;
};

NS_IMPL_ISUPPORTS1(ShowMediaListEnumerator ,
                   sbIMediaListEnumerationListener)


ShowMediaListEnumerator::ShowMediaListEnumerator(PRBool aHideMediaLists)
: mHideMediaLists(aHideMediaLists)
{
  mHideMediaListsStringValue = (mHideMediaLists == PR_TRUE) ?
                               NS_LITERAL_STRING("1") :
                               NS_LITERAL_STRING("0");
}

NS_IMETHODIMP ShowMediaListEnumerator::OnEnumerationBegin(sbIMediaList*,
                                                          PRUint16 *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = sbIMediaListEnumerationListener::CONTINUE;
  return NS_OK;
}

NS_IMETHODIMP ShowMediaListEnumerator::OnEnumeratedItem(sbIMediaList*,
                                                        sbIMediaItem* aItem,
                                                        PRUint16 *_retval)
{
  NS_ENSURE_ARG_POINTER(aItem);
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv = aItem->SetProperty(NS_LITERAL_STRING(SB_PROPERTY_HIDDEN),
                          mHideMediaListsStringValue);
  NS_ENSURE_SUCCESS(rv, rv);

  *_retval = sbIMediaListEnumerationListener::CONTINUE;

  return NS_OK;
}

NS_IMETHODIMP ShowMediaListEnumerator::OnEnumerationEnd(sbIMediaList*,
                                                        nsresult)
{
  return NS_OK;
}

sbBaseDevice::TransferRequest * sbBaseDevice::TransferRequest::New()
{
  return new TransferRequest();
}

PRBool sbBaseDevice::TransferRequest::IsPlaylist() const
{
  if (!list)
    return PR_FALSE;
  // Is this a playlist
  nsCOMPtr<sbILibrary> libTest = do_QueryInterface(list);
  return libTest ? PR_FALSE : PR_TRUE;
}

PRBool sbBaseDevice::TransferRequest::IsCountable() const
{
  return !IsPlaylist() &&
         type != sbIDevice::REQUEST_UPDATE;
}


/**
 * Utility function to check a transfer request queue for proper batching
 */
#if DEBUG
static void CheckRequestBatch(sbBaseDevice::TransferRequestQueue::const_iterator aBegin,
                              sbBaseDevice::TransferRequestQueue::const_iterator aEnd)
{
  PRUint32 lastIndex = 0, lastCount = 0, lastBatchID = 0;
  int lastType = 0;

  for (;aBegin != aEnd; ++aBegin) {
    if (!(*aBegin)->IsCountable()) {
      // we don't care about any non-countable things
      continue;
    }
    if (lastType == 0 || lastType != (*aBegin)->type) {
      // type change
      if (lastIndex != lastCount) {
        printf("Type change with missing items lastBatchID=%ud newBatchID=%ud\n",
               lastBatchID, (*aBegin)->batchID);
        NS_WARNING("Type change with missing items");
      }
      if ((lastType != 0) && ((*aBegin)->batchIndex != 1)) {
        printf ("batch does not start with 1 lastBatchID=%ud newBatchID=%ud\n",
                lastBatchID, (*aBegin)->batchID);
        NS_WARNING("batch does not start with 1");
      }
      if ((*aBegin)->batchCount == 0) {
        printf("empty batch lastBatchID=%ud newBatchID=%ud\n",
               lastBatchID, (*aBegin)->batchID);
        NS_WARNING("empty batch;");
      }
      lastType = (*aBegin)->type;
      lastCount = (*aBegin)->batchCount;
      lastIndex = (*aBegin)->batchIndex;
      lastBatchID = (*aBegin)->batchID;
    } else {
      // continue batch
      if (lastCount != (*aBegin)->batchCount) {
        printf("mismatched batch count "
               "batchID=%ud, batchCount=%ud, this Count=%ud, index=%ud\n",
               lastBatchID, lastCount, (*aBegin)->batchCount, (*aBegin)->batchIndex);
        NS_WARNING("mismatched batch count");
      }
      if (lastIndex + 1 != (*aBegin)->batchIndex) {
        printf("unexpected index batchID=%ud, lastIndex=%ud, index=%ud",
               lastBatchID, lastIndex, (*aBegin)->batchIndex);
        NS_WARNING("unexpected index");
      }
      lastIndex = (*aBegin)->batchIndex;
    }
  }

  // check end of queue too
  NS_ASSERTION(lastIndex == lastCount, "end of queue with missing items");
}
#endif /* DEBUG */

sbBaseDevice::sbBaseDevice() :
  mNextBatchID(1),
  mLastTransferID(0),
  mLastRequestPriority(PR_INT32_MIN),
  mAbortCurrentRequest(PR_FALSE),
  mIgnoreMediaListCount(0),
  mPerTrackOverhead(DEFAULT_PER_TRACK_OVERHEAD),
  mCapabilitiesRegistrarType(sbIDeviceCapabilitiesRegistrar::NONE),
  mPreferenceLock(nsnull),
  mMusicLimitPercent(100)
{
#ifdef PR_LOGGING
  if (!gBaseDeviceLog) {
    gBaseDeviceLog = PR_NewLogModule( "sbBaseDevice" );
  }
#endif /* PR_LOGGING */

  mStateLock = nsAutoLock::NewLock(__FILE__ "::mStateLock");
  NS_ASSERTION(mStateLock, "Failed to allocate state lock");

  mRequestMonitor = nsAutoMonitor::NewMonitor(__FILE__ "::mRequestMonitor");
  NS_ASSERTION(mRequestMonitor, "Failed to allocate request monitor");

  mRequestBatchTimerLock = nsAutoLock::NewLock(__FILE__ "::mRequestBatchTimerLock");
  NS_ASSERTION(mRequestBatchTimerLock, "Failed to allocate request batch timer lock");

  mPreferenceLock = nsAutoLock::NewLock(__FILE__ "::mPreferenceLock");
  NS_ASSERTION(mPreferenceLock, "Failed to allocate preference lock");
}

sbBaseDevice::~sbBaseDevice()
{
  if (mPreferenceLock)
    nsAutoLock::DestroyLock(mPreferenceLock);

  if (mRequestMonitor)
    nsAutoMonitor::DestroyMonitor(mRequestMonitor);

  if (mStateLock)
    nsAutoLock::DestroyLock(mStateLock);

  if (mRequestBatchTimerLock)
    nsAutoLock::DestroyLock(mRequestBatchTimerLock);
}

NS_IMETHODIMP sbBaseDevice::SyncLibraries()
{
  nsresult rv;

  nsCOMPtr<sbIDeviceContent> content;
  rv = GetContent(getter_AddRefs(content));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIArray> libraries;
  rv = content->GetLibraries(getter_AddRefs(libraries));
  NS_ENSURE_SUCCESS(rv, rv);

  PRUint32 libraryCount;
  rv = libraries->GetLength(&libraryCount);
  NS_ENSURE_SUCCESS(rv, rv);

  for (PRUint32 index = 0; index < libraryCount; ++index) {
    nsCOMPtr<sbIDeviceLibrary> deviceLib =
      do_QueryElementAt(libraries, index, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = deviceLib->Sync();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult sbBaseDevice::PushRequest(const int aType,
                                   sbIMediaItem* aItem,
                                   sbIMediaList* aList,
                                   PRUint32 aIndex,
                                   PRUint32 aOtherIndex,
                                   PRInt32 aPriority)
{
  NS_ENSURE_TRUE(mRequestMonitor, NS_ERROR_NOT_INITIALIZED);
  NS_ENSURE_TRUE(aType != 0, NS_ERROR_INVALID_ARG);

  nsRefPtr<TransferRequest> req = TransferRequest::New();
  NS_ENSURE_TRUE(req, NS_ERROR_OUT_OF_MEMORY);
  req->type = aType;
  req->item = aItem;
  req->list = aList;
  req->index = aIndex;
  req->otherIndex = aOtherIndex;
  req->priority = aPriority;

  return PushRequest(req);
}

/**
 * Finds the last countable item in the queue. If not found it returns
 * end
 */
template <class T>
inline
T SBFindLastCountable(T begin, T end)
{
  T const theEnd = end;

  if (begin != end) {
    // Start at the last item not "end" and find the first countable item
    for (--end; end != begin && !(*end)->IsCountable(); --end) ;
    // We'll either be at begin or some point between begin and end
    // If we're at the beginning then we may or may not have found a
    // countable item. So we have to test it
    return (*end)->IsCountable() ? end : theEnd;
  }
  // There are no items, so just return the "end"
  return theEnd;
}

/**
 * Updates the batch count for the request items associated with this batch
 */
template <class T>
void SBUpdateBatchCounts(T batchEnd, T queueBegin, PRUint32 aBatchCount, int aBatchID)
{
  // Reverse iterator from the end of the batch to the beginning and
  // bump the batch count, skipping the non-countable stuff
  for (;(!(*batchEnd)->IsCountable() || (*batchEnd)->batchID == aBatchID);
       --batchEnd) {
    if ((*batchEnd)->IsCountable()) {
      NS_ASSERTION((*batchEnd)->batchCount == aBatchCount - 1,
                   "Unexpected batch count in old request");
      ++((*batchEnd)->batchCount);
    }
    // Bail at beginning
    // Can't test in for statement since we'd miss the last one
    if (batchEnd == queueBegin) {
      break;
    }
  }
}

nsresult sbBaseDevice::PushRequest(TransferRequest *aRequest)
{
  NS_ENSURE_ARG_POINTER(aRequest);

  #if DEBUG
    // XXX Mook: if writing, make sure that we're writing from a file
    if (aRequest->type == TransferRequest::REQUEST_WRITE) {
      NS_ASSERTION(aRequest->item, "writing with no item");
      nsCOMPtr<nsIURI> aSourceURI;
      aRequest->item->GetContentSrc(getter_AddRefs(aSourceURI));
      PRBool schemeIs = PR_FALSE;
      aSourceURI->SchemeIs("file", &schemeIs);
      NS_ASSERTION(schemeIs, "writing from device, but not from file!");
    }
  #endif

  { /* scope for request lock */
    nsAutoMonitor requestMon(mRequestMonitor);

    // If we're aborting don't accept any more requests
    if (mAbortCurrentRequest)
    {
      return SB_ERROR_REQUEST_ABORTED;
    }
    /* decide where this request will be inserted */
    // figure out which queue we're looking at
    PRInt32 priority = aRequest->priority;
    TransferRequestQueue& queue = mRequests[priority];

    #if DEBUG
      CheckRequestBatch(queue.begin(), queue.end());
    #endif /* DEBUG */

    // initialize some properties of the request
    aRequest->itemTransferID = mLastTransferID++;
    aRequest->batchIndex = 1;
    aRequest->batchCount = 1;
    aRequest->batchID = 0;
    aRequest->timeStamp = PR_Now();

    // If this request isn't countable there's nothing to be done
    if (aRequest->IsCountable())
    {
      // Calculate the batch count
      TransferRequestQueue::iterator const begin = queue.begin();
      TransferRequestQueue::iterator const end = queue.end();
      TransferRequestQueue::iterator lastCountable = SBFindLastCountable(begin, end);

      // If there was a countable request found and it's the same type
      if (lastCountable != end && (*lastCountable)->type == aRequest->type) {
        nsRefPtr<TransferRequest> last = *(lastCountable);
        // batch them
        aRequest->batchCount += last->batchCount;
        aRequest->batchIndex = aRequest->batchCount;
        aRequest->batchID = last->batchID;

        SBUpdateBatchCounts(lastCountable, begin, aRequest->batchCount, aRequest->batchID);
      } else {
        // start of a new batch.  allocate a new batch ID atomically, ensuring
        // its value is never 0 (assuming it's not incremented 2^32-1 times
        // between the calls to PR_AtomicIncrement and that it's OK to sometimes
        // increment a few times too often)
        PRInt32 batchID = PR_AtomicIncrement(&mNextBatchID);
        if (!batchID)
          batchID = PR_AtomicIncrement(&mNextBatchID);
        aRequest->batchID = batchID;
      }
    }

    queue.push_back(aRequest);

    #if DEBUG
      CheckRequestBatch(queue.begin(), queue.end());
    #endif /* DEBUG */

  } /* end scope for request lock */

  return ProcessRequest();
}

nsresult sbBaseDevice::GetFirstRequest(PRBool aRemove,
                                       sbBaseDevice::TransferRequest** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  NS_ENSURE_TRUE(mRequestMonitor, NS_ERROR_NOT_INITIALIZED);

  nsAutoMonitor reqMon(mRequestMonitor);

  // Note: we shouldn't remove any empty queues from the map either, if we're
  // not going to pop the request.

  // try to find the last peeked request
  TransferRequestQueueMap::iterator mapIt =
    mRequests.find(mLastRequestPriority);
  if (mapIt == mRequests.end()) {
    mapIt = mRequests.begin();
  }

  while (mapIt != mRequests.end()) {
    // always pop the request from the first queue

    TransferRequestQueue& queue = mapIt->second;

    #if DEBUG
      CheckRequestBatch(queue.begin(), queue.end());
    #endif

    if (queue.empty()) {
      if (aRemove) {
        // this queue is empty, remove it
        mRequests.erase(mapIt);
        mapIt = mRequests.begin();
      } else {
        // go to the next queue
        ++mapIt;
      }
      continue;
    }

    nsRefPtr<TransferRequest> request = queue.front();

    if (aRemove) {
      // we want to pop the request
      queue.pop_front();
      if (queue.empty()) {
        mRequests.erase(mapIt);
        mapIt = mRequests.begin();
      } else {
        #if DEBUG
          CheckRequestBatch(queue.begin(), queue.end());
        #endif
      }
      // forget the last seen priority
      mLastRequestPriority = PR_INT32_MIN;
    } else {
      // record the last seen priority
      mLastRequestPriority = mapIt->first;
    }

    request.forget(_retval);
    return NS_OK;
  }
  // there are no queues left
  mLastRequestPriority = PR_INT32_MIN;
  *_retval = nsnull;
  return NS_OK;
}

nsresult sbBaseDevice::PopRequest(sbBaseDevice::TransferRequest** _retval)
{
  nsCOMPtr<nsIThread> thread;
  if (mRequestBatchTimer) {
    NS_GetCurrentThread(getter_AddRefs(thread));
    do {
      // Wait for an event before checking to see if the timer has fired
      NS_ProcessNextEvent(thread, PR_INTERVAL_NO_TIMEOUT);

    } while (mRequestBatchTimer);
  }

  return GetFirstRequest(PR_TRUE, _retval);
}

nsresult sbBaseDevice::PeekRequest(sbBaseDevice::TransferRequest** _retval)
{
  return GetFirstRequest(PR_FALSE, _retval);
}

template <class T>
NS_HIDDEN_(PRBool) Compare(T* left, nsCOMPtr<T> right)
{
  nsresult rv = NS_OK;
  PRBool isEqual = (left == right) ? PR_TRUE : PR_FALSE;
  if (!isEqual && left != nsnull && right != nsnull)
    rv = left->Equals(right, &isEqual);
  return NS_SUCCEEDED(rv) && isEqual;
}

nsresult sbBaseDevice::RemoveRequest(const int aType,
                                     sbIMediaItem* aItem,
                                     sbIMediaList* aList)
{
  NS_ENSURE_TRUE(mRequestMonitor, NS_ERROR_NOT_INITIALIZED);

  nsAutoMonitor reqMon(mRequestMonitor);

  // always pop the request from the first queue
  // can't just test for empty because we may have junk requests
  TransferRequestQueueMap::iterator mapIt = mRequests.begin(),
                                    mapEnd = mRequests.end();

  for (; mapIt != mapEnd; ++mapIt) {
    TransferRequestQueue& queue = mapIt->second;

    // more possibly dummy item accounting
    TransferRequestQueue::iterator queueIt = queue.begin(),
                                   queueEnd = queue.end();

    #if DEBUG
      CheckRequestBatch(queueIt, queueEnd);
    #endif

    for (; queueIt != queueEnd; ++queueIt) {
      nsRefPtr<TransferRequest> request = *queueIt;

      if (request->type == aType &&
          Compare(aItem, request->item) && Compare(aList, request->list))
      {
        // found; remove
        queue.erase(queueIt);

        #if DEBUG
          CheckRequestBatch(queue.begin(), queue.end());
        #endif /* DEBUG */

        return NS_OK;
      }
    }

    #if DEBUG
      CheckRequestBatch(queue.begin(), queue.end());
    #endif /* DEBUG */

  }

  // there are no queues left
  return NS_SUCCESS_LOSS_OF_INSIGNIFICANT_DATA;
}

typedef std::vector<nsRefPtr<sbBaseDevice::TransferRequest> > sbBaseDeviceTransferRequests;

/**
 * This iterates over the transfer requests and removes the Songbird library
 * items that were created for the requests.
 */
static nsresult RemoveLibraryItems(sbBaseDeviceTransferRequests const & items, sbBaseDevice *aBaseDevice)
{
  sbBaseDeviceTransferRequests::const_iterator const end = items.end();
  for (sbBaseDeviceTransferRequests::const_iterator iter = items.begin();
       iter != end;
       ++iter) {
    nsRefPtr<sbBaseDevice::TransferRequest> request = *iter;
    // If this is a request that adds an item to the device we need to remove
    // it from the device since it never was copied
    if (request->type == sbBaseDevice::TransferRequest::REQUEST_WRITE) {
      if (request->list && request->item) {
        nsresult rv = aBaseDevice->DeleteItem(request->list, request->item);
        NS_ENSURE_SUCCESS(rv, rv);
      }
    }
  }
  return NS_OK;
}

nsresult sbBaseDevice::ClearRequests(const nsAString &aDeviceID)
{
  nsresult rv;
  sbBaseDeviceTransferRequests requests;

  nsRefPtr<TransferRequest> request;
  PeekRequest(getter_AddRefs(request));

  NS_ENSURE_TRUE(mRequestMonitor, NS_ERROR_NOT_INITIALIZED);
  {
    nsAutoMonitor reqMon(mRequestMonitor);

    if(!mRequests.empty()) {
      rv = SetState(STATE_CANCEL);
      NS_ENSURE_SUCCESS(rv, rv);

      // Save off the library items that are pending to avoid any
      // potential reenterancy issues when deleting them.
      TransferRequestQueueMap::const_iterator mapIt = mRequests.begin(),
                                              mapEnd = mRequests.end();
      for (; mapIt != mapEnd; ++mapIt) {
        const TransferRequestQueue& queue = mapIt->second;
        TransferRequestQueue::const_iterator queueIt = queue.begin(),
                                             queueEnd = queue.end();

        #if DEBUG
          CheckRequestBatch(queueIt, queueEnd);
        #endif

        for (; queueIt != queueEnd; ++queueIt) {
          if ((*queueIt)->type == sbBaseDevice::TransferRequest::REQUEST_WRITE) {
            requests.push_back(*queueIt);
          }
        }

        #if DEBUG
          CheckRequestBatch(queue.begin(), queue.end());
        #endif
      }

      mAbortCurrentRequest = PR_TRUE;
      mRequests.clear();
    }
  }

  // must not hold onto the monitor while we create sbDeviceStatus objects
  // because that involves proxying to the main thread

  if (!requests.empty()) {
    rv = RemoveLibraryItems(requests, this);
    NS_ENSURE_SUCCESS(rv, rv);

    if (request) {
      nsCOMPtr<nsIWritableVariant> var =
        do_CreateInstance("@songbirdnest.com/Songbird/Variant;1", &rv);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = var->SetAsISupports(request->item);
      NS_ENSURE_SUCCESS(rv, rv);

      CreateAndDispatchEvent(sbIDeviceEvent::EVENT_DEVICE_TRANSFER_END,
                             var,
                             PR_TRUE);
    }
  }

  return NS_OK;
}

/* nsIVariant getPreference (in AString aPrefName); */
NS_IMETHODIMP sbBaseDevice::GetPreference(const nsAString & aPrefName, nsIVariant **_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  NS_ENSURE_FALSE(aPrefName.IsEmpty(), NS_ERROR_INVALID_ARG);
  nsresult rv;

  // get the pref branch for this device
  nsCOMPtr<nsIPrefBranch> prefBranch;
  rv = GetPrefBranch(getter_AddRefs(prefBranch));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ConvertUTF16toUTF8 prefNameUTF8(aPrefName);

  // get tht type of the pref
  PRInt32 prefType;
  rv = prefBranch->GetPrefType(prefNameUTF8.get(), &prefType);
  NS_ENSURE_SUCCESS(rv, rv);

  // create a writable variant
  nsCOMPtr<nsIWritableVariant> writableVariant =
    do_CreateInstance("@songbirdnest.com/Songbird/Variant;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // get the value of our pref
  switch (prefType) {
    case nsIPrefBranch::PREF_INVALID: {
      rv = writableVariant->SetAsEmpty();
      NS_ENSURE_SUCCESS(rv, rv);
      break;
    }
    case nsIPrefBranch::PREF_STRING: {
      char* _value = NULL;
      rv = prefBranch->GetCharPref(prefNameUTF8.get(), &_value);
      NS_ENSURE_SUCCESS(rv, rv);

      nsCString value;
      value.Adopt(_value);

      // set the value of the variant to the value of the pref
      rv = writableVariant->SetAsACString(value);
      NS_ENSURE_SUCCESS(rv, rv);
      break;
    }
    case nsIPrefBranch::PREF_INT: {
      PRInt32 value;
      rv = prefBranch->GetIntPref(prefNameUTF8.get(), &value);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = writableVariant->SetAsInt32(value);
      NS_ENSURE_SUCCESS(rv, rv);
      break;
    }
    case nsIPrefBranch::PREF_BOOL: {
      PRBool value;
      rv = prefBranch->GetBoolPref(prefNameUTF8.get(), &value);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = writableVariant->SetAsBool(value);
      NS_ENSURE_SUCCESS(rv, rv);
      break;
    }
  }

  return CallQueryInterface(writableVariant, _retval);
}

/* void setPreference (in AString aPrefName, in nsIVariant aPrefValue); */
NS_IMETHODIMP sbBaseDevice::SetPreference(const nsAString & aPrefName, nsIVariant *aPrefValue)
{
  NS_ENSURE_ARG_POINTER(aPrefValue);
  NS_ENSURE_FALSE(aPrefName.IsEmpty(), NS_ERROR_INVALID_ARG);
  nsresult rv;

  PRBool hasChanged = PR_FALSE;
  rv = SetPreferenceInternal(aPrefName, aPrefValue, &hasChanged);
  NS_ENSURE_SUCCESS(rv, rv);

  if (hasChanged) {
    // apply the preference
    ApplyPreference(aPrefName, aPrefValue);

    // fire the pref change event
    nsCOMPtr<sbIDeviceManager2> devMgr =
      do_GetService("@songbirdnest.com/Songbird/DeviceManager;2", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<sbIDeviceEvent> event;
    rv = devMgr->CreateEvent(sbIDeviceEvent::EVENT_DEVICE_PREFS_CHANGED,
                             nsnull, nsnull, getter_AddRefs(event));
    NS_ENSURE_SUCCESS(rv, rv);

    PRBool success;
    rv = this->DispatchEvent(event, PR_FALSE, &success);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult sbBaseDevice::SetPreferenceInternal(const nsAString& aPrefName,
                                             nsIVariant*      aPrefValue,
                                             PRBool*          aHasChanged)
{
  NS_ENSURE_ARG_POINTER(aPrefValue);
  NS_ENSURE_FALSE(aPrefName.IsEmpty(), NS_ERROR_INVALID_ARG);
  nsresult rv;

  // get the pref branch for this device
  nsCOMPtr<nsIPrefBranch> prefBranch;
  rv = GetPrefBranch(getter_AddRefs(prefBranch));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ConvertUTF16toUTF8 prefNameUTF8(aPrefName);

  // figure out what sort of variant we have
  PRUint16 dataType;
  rv = aPrefValue->GetDataType(&dataType);
  NS_ENSURE_SUCCESS(rv, rv);

  // figure out what sort of data we used to have
  PRInt32 prefType;
  rv = prefBranch->GetPrefType(prefNameUTF8.get(), &prefType);
  NS_ENSURE_SUCCESS(rv, rv);

  PRBool hasChanged = PR_FALSE;

  switch (dataType) {
    case nsIDataType::VTYPE_INT8:
    case nsIDataType::VTYPE_INT16:
    case nsIDataType::VTYPE_INT32:
    case nsIDataType::VTYPE_INT64:
    case nsIDataType::VTYPE_UINT8:
    case nsIDataType::VTYPE_UINT16:
    case nsIDataType::VTYPE_UINT32:
    case nsIDataType::VTYPE_UINT64:
    case nsIDataType::VTYPE_FLOAT:
    case nsIDataType::VTYPE_DOUBLE:
    {
      // some sort of number
      PRInt32 oldValue, value;
      rv = aPrefValue->GetAsInt32(&value);
      NS_ENSURE_SUCCESS(rv, rv);

      if (prefType != nsIPrefBranch::PREF_INT) {
        hasChanged = PR_TRUE;
      } else {
        rv = prefBranch->GetIntPref(prefNameUTF8.get(), &oldValue);
        if (NS_SUCCEEDED(rv) && oldValue != value) {
          hasChanged = PR_TRUE;
        }
      }

      rv = prefBranch->SetIntPref(prefNameUTF8.get(), value);
      NS_ENSURE_SUCCESS(rv, rv);

      break;
    }

    case nsIDataType::VTYPE_BOOL:
    {
      // a bool pref
      PRBool oldValue, value;
      rv = aPrefValue->GetAsBool(&value);
      NS_ENSURE_SUCCESS(rv, rv);

      if (prefType != nsIPrefBranch::PREF_BOOL) {
        hasChanged = PR_TRUE;
      } else {
        rv = prefBranch->GetBoolPref(prefNameUTF8.get(), &oldValue);
        if (NS_SUCCEEDED(rv) && oldValue != value) {
          hasChanged = PR_TRUE;
        }
      }

      rv = prefBranch->SetBoolPref(prefNameUTF8.get(), value);
      NS_ENSURE_SUCCESS(rv, rv);

      break;
    }

    case nsIDataType::VTYPE_VOID:
    case nsIDataType::VTYPE_EMPTY:
    {
      // unset the pref
      if (prefType != nsIPrefBranch::PREF_INVALID) {
        rv = prefBranch->ClearUserPref(prefNameUTF8.get());
        NS_ENSURE_SUCCESS(rv, rv);
        hasChanged = PR_TRUE;
      }

      break;
    }

    default:
    {
      // assume a string
      nsCString value;
      rv = aPrefValue->GetAsACString(value);
      NS_ENSURE_SUCCESS(rv, rv);

      if (prefType != nsIPrefBranch::PREF_STRING) {
        hasChanged = PR_TRUE;
      } else {
        char* oldValue;
        rv = prefBranch->GetCharPref(prefNameUTF8.get(), &oldValue);
        if (NS_SUCCEEDED(rv)) {
          if (!(value.Equals(oldValue))) {
            hasChanged = PR_TRUE;
          }
          NS_Free(oldValue);
        }
      }

      rv = prefBranch->SetCharPref(prefNameUTF8.get(), value.get());
      NS_ENSURE_SUCCESS(rv, rv);

      break;
    }
  }

  // return has changed status
  if (aHasChanged)
    *aHasChanged = hasChanged;

  return NS_OK;
}

/* readonly attribute boolean isBusy; */
NS_IMETHODIMP sbBaseDevice::GetIsBusy(PRBool *aIsBusy)
{
  NS_ENSURE_ARG_POINTER(aIsBusy);
  NS_ENSURE_TRUE(mStateLock, NS_ERROR_NOT_INITIALIZED);
  nsAutoLock lock(mStateLock);
  switch (mState) {
    case STATE_IDLE:
    case STATE_DOWNLOAD_PAUSED:
    case STATE_UPLOAD_PAUSED:
      *aIsBusy = PR_FALSE;
    break;

    default:
      *aIsBusy = PR_TRUE;
    break;
  }
  return NS_OK;
}

/* readonly attribute boolean canDisconnect; */
NS_IMETHODIMP sbBaseDevice::GetCanDisconnect(PRBool *aCanDisconnect)
{
  NS_ENSURE_ARG_POINTER(aCanDisconnect);
  NS_ENSURE_TRUE(mStateLock, NS_ERROR_NOT_INITIALIZED);
  nsAutoLock lock(mStateLock);
  switch(mState) {
    case STATE_IDLE:
    case STATE_MOUNTING:
    case STATE_DISCONNECTED:
    case STATE_DOWNLOAD_PAUSED:
    case STATE_UPLOAD_PAUSED:
      *aCanDisconnect = PR_TRUE;
    break;

    default:
      *aCanDisconnect = PR_FALSE;
    break;
  }
  return NS_OK;
}

/* readonly attribute unsigned long state; */
NS_IMETHODIMP sbBaseDevice::GetState(PRUint32 *aState)
{
  NS_ENSURE_ARG_POINTER(aState);
  NS_ENSURE_TRUE(mStateLock, NS_ERROR_NOT_INITIALIZED);
  nsAutoLock lock(mStateLock);
  *aState = mState;
  return NS_OK;
}

nsresult sbBaseDevice::SetState(PRUint32 aState)
{

  nsresult rv;
  PRBool stateChanged = PR_FALSE;

  // set state, checking if it changed
  {
    NS_ENSURE_TRUE(mStateLock, NS_ERROR_NOT_INITIALIZED);
    nsAutoLock lock(mStateLock);
    if (mState != aState) {
      mState = aState;
      stateChanged = PR_TRUE;
    }
  }

  // send state changed event.  do it outside of lock in case event handler gets
  // called immediately and tries to read the state
  if (stateChanged) {
    nsCOMPtr<nsIWritableVariant> var = do_CreateInstance("@songbirdnest.com/Songbird/Variant;1",
                                                         &rv);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = var->SetAsUint32(aState);
    NS_ENSURE_SUCCESS(rv, rv);
    CreateAndDispatchEvent(sbIDeviceEvent::EVENT_DEVICE_STATE_CHANGED, var);
  }

  return NS_OK;
}

nsresult sbBaseDevice::CreateDeviceLibrary(const nsAString& aId,
                                           nsIURI* aLibraryLocation,
                                           sbIDeviceLibrary** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsRefPtr<sbDeviceLibrary> devLib = new sbDeviceLibrary(this);
  NS_ENSURE_TRUE(devLib, NS_ERROR_OUT_OF_MEMORY);

  nsresult rv = InitializeDeviceLibrary(devLib, aId, aLibraryLocation);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = devLib->QueryInterface(NS_GET_IID(sbIDeviceLibrary),
                              reinterpret_cast<void**>(_retval));
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::InitializeDeviceLibrary
                         (sbDeviceLibrary* aDevLib,
                          const nsAString& aId,
                          nsIURI*          aLibraryLocation)
{
  NS_ENSURE_ARG_POINTER(aDevLib);

  if (!mMediaListListeners.IsInitialized()) {
    // we expect to be unintialized, but just in case...
    if (!mMediaListListeners.Init()) {
      return NS_ERROR_FAILURE;
    }
  }

  nsresult rv = aDevLib->Initialize(aId);
  NS_ENSURE_SUCCESS(rv, rv);

  // Hide the library on creation. The device is responsible
  // for showing it is done mounting.
  rv = aDevLib->SetProperty(NS_LITERAL_STRING(SB_PROPERTY_HIDDEN),
                            NS_LITERAL_STRING("1"));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aDevLib->SetProperty(NS_LITERAL_STRING(SB_PROPERTY_ISSORTABLE),
                            NS_LITERAL_STRING("1"));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aDevLib->SetProperty(NS_LITERAL_STRING(SB_PROPERTY_DEFAULTCOLUMNSPEC),
                            NS_ConvertASCIItoUTF16(NS_LITERAL_CSTRING(DEFAULT_COLUMNSPEC_DEVICE_LIBRARY)));
  NS_ENSURE_SUCCESS(rv, rv);

  nsRefPtr<sbBaseDeviceLibraryListener> libListener = new sbBaseDeviceLibraryListener();
  NS_ENSURE_TRUE(libListener, NS_ERROR_OUT_OF_MEMORY);

  rv = libListener->Init(this);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aDevLib->AddDeviceLibraryListener(libListener);
  NS_ENSURE_SUCCESS(rv, rv);

  // hook up the media list listeners to the existing lists
  nsRefPtr<MediaListListenerAttachingEnumerator> enumerator =
    new MediaListListenerAttachingEnumerator(this);
  NS_ENSURE_TRUE(enumerator, NS_ERROR_OUT_OF_MEMORY);

  rv = aDevLib->EnumerateItemsByProperty(NS_LITERAL_STRING(SB_PROPERTY_ISLIST),
                                         NS_LITERAL_STRING("1"),
                                         enumerator,
                                         sbIMediaList::ENUMERATIONTYPE_SNAPSHOT);
  NS_ENSURE_SUCCESS(rv, rv);

  libListener.swap(mLibraryListener);

  return NS_OK;
}

void sbBaseDevice::FinalizeDeviceLibrary(sbIDeviceLibrary* aDevLib)
{
  // Finalize and clear the media list listeners.
  EnumerateFinalizeMediaListListenersInfo enumerateInfo;
  enumerateInfo.device = this;
  enumerateInfo.library = aDevLib;
  mMediaListListeners.Enumerate
                        (sbBaseDevice::EnumerateFinalizeMediaListListeners,
                         &enumerateInfo);

  // Finalize the device library.
  aDevLib->Finalize();
}

nsresult sbBaseDevice::AddLibrary(sbIDeviceLibrary* aDevLib)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aDevLib);

  // Function variables.
  nsresult rv;

  // Get the device content.
  nsCOMPtr<sbIDeviceContent> content;
  rv = GetContent(getter_AddRefs(content));
  NS_ENSURE_SUCCESS(rv, rv);

  // Add the library to the device content.
  rv = content->AddLibrary(aDevLib);
  NS_ENSURE_SUCCESS(rv, rv);

  // Send a device library added event.
  CreateAndDispatchEvent(sbIDeviceEvent::EVENT_DEVICE_LIBRARY_ADDED,
                         sbNewVariant(aDevLib));

  // Apply the library preferences.
  rv = ApplyLibraryPreference(aDevLib, SBVoidString(), nsnull);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::RemoveLibrary(sbIDeviceLibrary* aDevLib)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aDevLib);

  // Function variables.
  nsresult rv;

  // Send a device library removed event.
  nsAutoString guid;
  rv = aDevLib->GetGuid(guid);
  NS_ASSERTION(NS_SUCCEEDED(rv), "Failed to get device library.");
  CreateAndDispatchEvent(sbIDeviceEvent::EVENT_DEVICE_LIBRARY_REMOVED,
                         sbNewVariant(guid));

  // Get the device content.
  nsCOMPtr<sbIDeviceContent> content;
  rv = GetContent(getter_AddRefs(content));
  NS_ENSURE_SUCCESS(rv, rv);

  // Remove the device library from the device content.
  rv = content->RemoveLibrary(aDevLib);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::ListenToList(sbIMediaList* aList)
{
  NS_ENSURE_ARG_POINTER(aList);

  NS_ASSERTION(mMediaListListeners.IsInitialized(),
               "sbBaseDevice::ListenToList called before listener hash is initialized!");

  nsresult rv;

  #if DEBUG
    // check to make sure we're not listening to a library
    nsCOMPtr<sbILibrary> library = do_QueryInterface(aList);
    NS_ASSERTION(!library,
                 "Should not call sbBaseDevice::ListenToList on a library!");
  #endif

  // the extra QI to make sure we're at the canonical pointer
  // and not some derived interface
  nsCOMPtr<sbIMediaList> list = do_QueryInterface(aList, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // check for an existing listener
  if (mMediaListListeners.Get(list, nsnull)) {
    // we are already listening to the media list, don't re-add
    return NS_OK;
  }

  nsRefPtr<sbBaseDeviceMediaListListener> listener =
    new sbBaseDeviceMediaListListener();
  NS_ENSURE_TRUE(listener, NS_ERROR_OUT_OF_MEMORY);

  rv = listener->Init(this);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = list->AddListener(listener,
                         PR_FALSE, /* weak */
                         0, /* all */
                         nsnull /* filter */);
  NS_ENSURE_SUCCESS(rv, rv);

  // If we're currently ignoring listeners then we need to ignore this one too
  // else we'll get out of balance
  if (mIgnoreMediaListCount > 0)
    listener->SetIgnoreListener(PR_TRUE);
  mMediaListListeners.Put(list, listener);

  return NS_OK;
}

PLDHashOperator sbBaseDevice::EnumerateFinalizeMediaListListeners
                                (nsISupports* aKey,
                                 nsRefPtr<sbBaseDeviceMediaListListener>& aData,
                                 void* aClosure)
{
  nsresult rv;

  // Get the device and library for which to finalize media list listeners.
  EnumerateFinalizeMediaListListenersInfo*
    enumerateInfo =
      static_cast<EnumerateFinalizeMediaListListenersInfo*>(aClosure);
  nsCOMPtr<sbILibrary> library = enumerateInfo->library;

  // Get the listener media list.
  nsCOMPtr<sbIMediaList> mediaList = do_QueryInterface(aKey, &rv);
  NS_ENSURE_SUCCESS(rv, PL_DHASH_STOP);

  // Do nothing if media list is contained in another library.
  nsCOMPtr<sbILibrary> mediaListLibrary;
  PRBool               equals;
  rv = mediaList->GetLibrary(getter_AddRefs(mediaListLibrary));
  NS_ENSURE_SUCCESS(rv, PL_DHASH_STOP);
  rv = mediaListLibrary->Equals(library, &equals);
  NS_ENSURE_SUCCESS(rv, PL_DHASH_STOP);
  if (!equals)
    return PL_DHASH_NEXT;

  // Remove the media list listener.
  mediaList->RemoveListener(aData);

  return PL_DHASH_REMOVE;
}

PLDHashOperator sbBaseDevice::EnumerateIgnoreMediaListListeners(nsISupports* aKey,
                                                                nsRefPtr<sbBaseDeviceMediaListListener> aData,
                                                                void* aClosure)
{
  nsresult rv;
  PRBool *ignore = static_cast<PRBool *>(aClosure);

  rv = aData->SetIgnoreListener(*ignore);
  NS_ENSURE_SUCCESS(rv, PL_DHASH_STOP);

  return PL_DHASH_NEXT;
}

nsresult
sbBaseDevice::SetIgnoreMediaListListeners(PRBool aIgnoreListener)
{
  if (aIgnoreListener)
    PR_AtomicIncrement(&mIgnoreMediaListCount);
  else
    PR_AtomicDecrement(&mIgnoreMediaListCount);

  mMediaListListeners.EnumerateRead(sbBaseDevice::EnumerateIgnoreMediaListListeners,
                                    &aIgnoreListener);
  return NS_OK;
}

nsresult
sbBaseDevice::SetIgnoreLibraryListener(PRBool aIgnoreListener)
{
  return mLibraryListener->SetIgnoreListener(aIgnoreListener);
}

nsresult
sbBaseDevice::SetMediaListsHidden(sbIMediaList *aLibrary, PRBool aHidden)
{
  NS_ENSURE_ARG_POINTER(aLibrary);

  nsRefPtr<ShowMediaListEnumerator> enumerator = new ShowMediaListEnumerator(aHidden);
  NS_ENSURE_TRUE(enumerator, NS_ERROR_OUT_OF_MEMORY);

  nsresult rv = aLibrary->EnumerateItemsByProperty(NS_LITERAL_STRING(SB_PROPERTY_ISLIST),
                                                   NS_LITERAL_STRING("1"),
                                                   enumerator,
                                                   sbIMediaList::ENUMERATIONTYPE_SNAPSHOT);
  return rv;
}

nsresult sbBaseDevice::IgnoreMediaItem(sbIMediaItem * aItem) {
  return mLibraryListener->IgnoreMediaItem(aItem);
}

nsresult sbBaseDevice::UnignoreMediaItem(sbIMediaItem * aItem) {
  return mLibraryListener->UnignoreMediaItem(aItem);
}

nsresult
sbBaseDevice::DeleteItem(sbIMediaList *aLibrary, sbIMediaItem *aItem)
{
  NS_ENSURE_ARG_POINTER(aLibrary);
  NS_ENSURE_ARG_POINTER(aItem);

  NS_ENSURE_STATE(mLibraryListener);

  SetIgnoreMediaListListeners(PR_TRUE);
  mLibraryListener->SetIgnoreListener(PR_TRUE);

  nsresult rv = aLibrary->Remove(aItem);

  SetIgnoreMediaListListeners(PR_FALSE);
  mLibraryListener->SetIgnoreListener(PR_FALSE);

  return rv;
}

nsresult
sbBaseDevice::CreateTransferRequest(PRUint32 aRequest,
                                    nsIPropertyBag2 *aRequestParameters,
                                    TransferRequest **aTransferRequest)
{
  NS_ENSURE_ARG_RANGE(aRequest, REQUEST_MOUNT, REQUEST_FACTORY_RESET);
  NS_ENSURE_ARG_POINTER(aRequestParameters);
  NS_ENSURE_ARG_POINTER(aTransferRequest);

  TransferRequest* req = TransferRequest::New();
  NS_ENSURE_TRUE(req, NS_ERROR_OUT_OF_MEMORY);

  nsresult rv;

  nsCOMPtr<sbIMediaItem> item;
  nsCOMPtr<sbIMediaList> list;
  nsCOMPtr<nsISupports>  data;

  PRUint32 index = PR_UINT32_MAX;
  PRUint32 otherIndex = PR_UINT32_MAX;
  PRInt32 priority = TransferRequest::PRIORITY_DEFAULT;

  rv = aRequestParameters->GetPropertyAsInterface(NS_LITERAL_STRING("item"),
                                                  NS_GET_IID(sbIMediaItem),
                                                  getter_AddRefs(item));
  NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "No item present in request parameters.");

  rv = aRequestParameters->GetPropertyAsInterface(NS_LITERAL_STRING("list"),
                                                  NS_GET_IID(sbIMediaList),
                                                  getter_AddRefs(list));
  NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "No list present in request parameters.");

  rv = aRequestParameters->GetPropertyAsInterface(NS_LITERAL_STRING("data"),
                                                  NS_GET_IID(nsISupports),
                                                  getter_AddRefs(data));
  NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "No data present in request parameters.");

  NS_WARN_IF_FALSE(item || list || data, "No data of any kind given in request. This request will most likely fail.");

  rv = aRequestParameters->GetPropertyAsUint32(NS_LITERAL_STRING("index"),
                                               &index);
  if(NS_FAILED(rv)) {
    index = PR_UINT32_MAX;
  }

  rv = aRequestParameters->GetPropertyAsUint32(NS_LITERAL_STRING("otherIndex"),
                                               &otherIndex);
  if(NS_FAILED(rv)) {
    otherIndex = PR_UINT32_MAX;
  }

  rv = aRequestParameters->GetPropertyAsInt32(NS_LITERAL_STRING("priority"),
                                               &priority);
  if(NS_FAILED(rv)) {
    priority = TransferRequest::PRIORITY_DEFAULT;
  }

  req->type = aRequest;
  req->item = item;
  req->list = list;
  req->data = data;
  req->index = index;
  req->otherIndex = otherIndex;
  req->priority = priority;

  NS_ADDREF(*aTransferRequest = req);

  return NS_OK;
}

nsresult sbBaseDevice::CreateAndDispatchEvent(PRUint32 aType,
                                              nsIVariant *aData,
                                              PRBool aAsync /*= PR_TRUE*/)
{
  nsresult rv;

  nsCOMPtr<sbIDeviceManager2> manager =
    do_GetService("@songbirdnest.com/Songbird/DeviceManager;2", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<sbIDeviceEvent> deviceEvent;
  rv = manager->CreateEvent(aType, aData, static_cast<sbIDevice*>(this),
                            getter_AddRefs(deviceEvent));
  NS_ENSURE_SUCCESS(rv, rv);

  return DispatchEvent(deviceEvent, aAsync, nsnull);
}

// by COM rules, we are only guaranteed that comparing nsISupports* is valid
struct COMComparator {
  bool operator()(nsISupports* aLeft, nsISupports* aRight) const {
    return nsCOMPtr<nsISupports>(do_QueryInterface(aLeft)) <
             nsCOMPtr<nsISupports>(do_QueryInterface(aRight));
  }
};

struct EnsureSpaceForWriteRemovalHelper {
  EnsureSpaceForWriteRemovalHelper(std::set<sbIMediaItem*,COMComparator>& aItemsToRemove)
    : mItemsToRemove(aItemsToRemove.begin(), aItemsToRemove.end())
  {
  }

  bool operator() (nsRefPtr<sbBaseDevice::TransferRequest> & aRequest) {
    // remove all requests where either the item or the list is to be removed
    return (mItemsToRemove.end() != mItemsToRemove.find(aRequest->item) ||
            mItemsToRemove.end() != mItemsToRemove.find(aRequest->list));
  }

  std::set<sbIMediaItem*, COMComparator> mItemsToRemove;
};

nsresult sbBaseDevice::EnsureSpaceForWrite(TransferRequest* aRequest,
                                           PRBool*          aWait)
{
  NS_ENSURE_ARG_POINTER(aRequest);
  NS_ENSURE_ARG_POINTER(aWait);

  nsresult rv;

  // default to not wait
  *aWait = PR_FALSE;

  // non-write and playlist requests are always ensured space
  if ((aRequest->type != TransferRequest::REQUEST_WRITE) ||
      (aRequest->IsPlaylist())) {
    aRequest->spaceEnsured = PR_TRUE;
    return NS_OK;
  }

  nsAutoMonitor monitor(mRequestMonitor);

  // get the request's queue
  TransferRequestQueue& queue = mRequests[aRequest->priority];

  // find the end of the batch
  nsRefPtr<TransferRequest> batchEndRequest;
  TransferRequestQueue::iterator batchEndIter;
  PRUint32 batchID = aRequest->batchID;
  for (batchEndIter = queue.begin();
       batchEndIter != queue.end();
       batchEndIter++) {
    // get the next request
    TransferRequest* request = *batchEndIter;

    // check for the target request
    if (request == aRequest) {
      batchEndRequest = request;
      continue;
    }

    // skip requests that are not a part of a batch
    if (!request->batchID)
      continue;

    // if part of same batch, move the batch end; otherwise, exit loop
    if (request->batchID == batchID)
      batchEndRequest = request;
    else
      break;
  }

  // if the request at the end of the batch was not found, the specified request
  // is not in the queue, and that's a failure
  NS_ENSURE_TRUE(batchEndRequest, NS_ERROR_FAILURE);

  // if complete batch is not yet in queue, wait
  if ((PR_Now() - batchEndRequest->timeStamp) <
      (BATCH_TIMEOUT * PR_USEC_PER_MSEC)) {

    // Leave the request monitor else we'll deadlock waiting
    monitor.Exit();
    rv = WaitForBatchEnd();
    NS_ENSURE_SUCCESS(rv, rv);
    *aWait = PR_TRUE;
    return NS_OK;
  }

  // ensure space for the write batch starting at the beginning of the queue
  mRequestBatchStart = *(queue.begin());
  rv = EnsureSpaceForWrite(queue);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

/**
 * This class is basically the monitor counter part to the sbAutoUnlock class
 * It leaves the monitor on construction and reenters it on destruction.
 */
class sbAutoMonitorUnlock : private nsAutoUnlockBase
{
public:
  sbAutoMonitorUnlock(PRMonitor *monitor) :
      nsAutoUnlockBase(monitor),
      mMonitor(monitor)
  {
    NS_ASSERTION(mMonitor, "sbAutoUnlockMonitor must be passed a valid monitor");
      PR_ExitMonitor(mMonitor);
  }

  ~sbAutoMonitorUnlock() {
      PR_EnterMonitor(mMonitor);
  }
private:
   PRMonitor *mMonitor;
};

template <class T>
inline
T find_iterator(T start, T end, T target)
{
  while (start != target && start != end) {
    ++start;
  }
  return start;
}

nsresult sbBaseDevice::EnsureSpaceForWrite(TransferRequestQueue& aQueue)
{
  LOG(("                        sbBaseDevice::EnsureSpaceForWrite++\n"));

  // We will possibly want to remove some writes.  Unfortunately, this may be
  // followed by some other actions (add-to-list, etc.) dealing with those items.
  // and yet we need to keep everything that does get executed in the same order
  // as the original queue.
  // So, we keep a std::map where the key is all media items to be written
  // (since writing the same item twice should not result in the space being
  // used twice).  For convenience later, the value is the size of the file.

  nsresult rv;
  std::map<sbIMediaItem*, PRInt64> itemsToWrite; // not owning
  // all items in itemsToWrite should have a reference from the request queue
  // XXX Mook: Do we need to be holding nsISupports* instead?

  // we have a batch that has some sort of write
  // check to see if it will all fit on the device
  TransferRequestQueue::iterator batchStart = aQueue.begin();
  TransferRequestQueue::iterator end = aQueue.end();

  // shift batchStart to where we last were
  while (batchStart != end && *batchStart != mRequestBatchStart)
    ++batchStart;

  // get the total size of the request
  PRInt64 contentLength, totalLength = 0;
  nsCOMPtr<sbIDeviceLibrary> ownerLibrary;

  TransferRequestQueue::iterator request;
  for (request = batchStart; request != end; ++request) {
    if ((*request)->type != TransferRequest::REQUEST_WRITE)
      continue;

    nsCOMPtr<sbILibrary> requestLib =
      do_QueryInterface((*request)->list, &rv);
    if (NS_FAILED(rv) || !requestLib) {
      // this is an add to playlist request, don't worry about size
      continue;
    }

    if (!ownerLibrary) {
      rv = sbDeviceUtils::GetDeviceLibraryForItem(this,
                                                  (*request)->list,
                                                  getter_AddRefs(ownerLibrary));
      NS_ENSURE_SUCCESS(rv, rv);
      NS_ENSURE_STATE(ownerLibrary);
    } else {
      #if DEBUG
        nsCOMPtr<sbIDeviceLibrary> newLibrary;
        rv = sbDeviceUtils::GetDeviceLibraryForItem(this,
                                                    (*request)->list,
                                                    getter_AddRefs(newLibrary));
        NS_ENSURE_SUCCESS(rv, rv);
        NS_ENSURE_STATE(newLibrary);
        NS_ENSURE_STATE(SameCOMIdentity(ownerLibrary, newLibrary));
      #endif /* DEBUG */
    }
    if (itemsToWrite.end() != itemsToWrite.find((*request)->item)) {
      // this item is already in the set, don't worry about it
      continue;
    }

    // Have to unlock for GetContentLength since it updates the length
    // if it's not on the item
    {
      sbAutoMonitorUnlock unlock(mRequestMonitor);

      rv = sbLibraryUtils::GetContentLength((*request)->item,
                                            &contentLength);
      NS_ENSURE_SUCCESS(rv, rv);
    }
    contentLength += mPerTrackOverhead;

    totalLength += contentLength;
    LOG(("r(%08x) i(%08x) sbBaseDevice::EnsureSpaceForWrite - size %lld\n",
         (void*) *request, (void*) (*request)->item, contentLength));

    itemsToWrite[(*request)->item] = contentLength;
  }

  LOG(("                        sbBaseDevice::EnsureSpaceForWrite - %u items = %lld bytes\n",
       itemsToWrite.size(), totalLength));

  if (itemsToWrite.size() < 1) {
    // there were no items to write (they were all add to playlist requests)
    return NS_OK;
  }

  // get the device properties
  nsCOMPtr<sbIDeviceProperties> baseDeviceProperties;
  nsCOMPtr<nsIPropertyBag2>     deviceProperties;
  rv = GetProperties(getter_AddRefs(baseDeviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = baseDeviceProperties->GetProperties(getter_AddRefs(deviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);

  // get the free space
  PRInt64 freeSpace;
  rv = deviceProperties->GetPropertyAsInt64
                           (NS_LITERAL_STRING(SB_DEVICE_PROPERTY_FREE_SPACE),
                            &freeSpace);
  NS_ENSURE_SUCCESS(rv, rv);

  // apply limit to the total space available for music
  if (ownerLibrary) {
    PRInt64 freeMusicSpace;
    rv = GetMusicFreeSpace(ownerLibrary, &freeMusicSpace);
    NS_ENSURE_SUCCESS(rv, rv);
    if (freeSpace >= freeMusicSpace)
      freeSpace = freeMusicSpace;
  }

  // get the management type
  PRUint32 mgmtType;
  PRBool continueWrite = PR_TRUE;
  { // Unlock so we don't deadlock with prefs branch and UI logic
    sbAutoMonitorUnlock unlock(mRequestMonitor);
    rv = ownerLibrary->GetMgmtType(&mgmtType);
    NS_ENSURE_SUCCESS(rv, rv);

    // if not enough free space is available, ask user what to do
    if (freeSpace < totalLength) {
      PRBool abort;
      rv = sbDeviceUtils::QueryUserSpaceExceeded
                            (this,
                             ownerLibrary,
                             totalLength,
                             freeSpace,
                             &abort);
      NS_ENSURE_SUCCESS(rv, rv);
      continueWrite = !abort;
    }
  }

  LOG(("                        sbBaseDevice::EnsureSpaceForWrite - %u items = %lld bytes / free %lld bytes\n",
       itemsToWrite.size(), totalLength, freeSpace));

  // there are three cases to go from here:
  // (1) fill up the device, with whatever fits, in random order (sync)
  // (2) fill up the device, with whatever fits, in given order (manual)
  // (3) user chose to abort

  // In the (1) case, if we first shuffle the list, it becomes case (2)
  // and case (2) is just case (3) with fewer files to remove

  // copy the set of items into a orderable list
  std::vector<sbIMediaItem*> itemList;
  std::map<sbIMediaItem*, PRInt64>::iterator itemsBegin = itemsToWrite.begin(),
                                             itemsEnd = itemsToWrite.end(),
                                             itemsNext;
  for (itemsNext = itemsBegin; itemsNext != itemsEnd; ++itemsNext) {
    itemList.push_back(itemsNext->first);
  }
  // and a set of items we can't fit
  std::set<sbIMediaItem*, COMComparator> itemsToRemove;

  if (!continueWrite) {
    // we need to remove everything
    itemsToRemove.insert(itemList.begin(), itemList.end());
    itemList.clear();
  } else {
    if (mgmtType != sbIDeviceLibrary::MGMT_TYPE_MANUAL) {
      // shuffle the list
      std::random_shuffle(itemList.begin(), itemList.end());
    }

    // fit as much of the list as possible
    std::vector<sbIMediaItem*>::iterator begin = itemList.begin(),
                                         end = itemList.end(),
                                         next;
    PRInt64 bytesRemaining = freeSpace;
    for (next = begin; next != end; ++next) {
      if (bytesRemaining > itemsToWrite[*next]) {
        // this will fit
        bytesRemaining -= itemsToWrite[*next];
      } else {
        // won't fit, remove it
        itemsToRemove.insert(*next);
        *next = nsnull; /* wipe out the sbIMediaItem* */
      }
    }
  }

  // locate all the items we don't want to copy. remove these items after loop
  // because removal can add requests to the queue and potentially invalidate
  // the iterators
  TransferRequestQueue requestItemRemoveQueue;
  std::set<sbIMediaItem*,COMComparator>::iterator removalEnd = itemsToRemove.end();
  for (request = batchStart; request != end; ++request) {
    if ((*request)->type != TransferRequest::REQUEST_WRITE ||
        removalEnd == itemsToRemove.find((*request)->item))
    {
      // not a write we don't want
      (*request)->spaceEnsured = PR_TRUE;
      continue;
    }

    requestItemRemoveQueue.push_back(*request);

    // tell the user about it
    nsCOMPtr<nsIWritableVariant> var =
      do_CreateInstance("@songbirdnest.com/Songbird/Variant;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = var->SetAsISupports((*request)->item);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = CreateAndDispatchEvent(sbIDeviceEvent::EVENT_DEVICE_NOT_ENOUGH_FREESPACE,
                                var,
                                PR_TRUE);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  { // Unlock so we don't deadlock during the Remove call when it pushes removes
    sbAutoMonitorUnlock unlock(mRequestMonitor);
    // remove all the items we don't want to copy from the songbird device library
    TransferRequestQueue::iterator requestItemRemoveIter =
                                     requestItemRemoveQueue.begin();
    TransferRequestQueue::iterator requestItemRemoveEnd =
                                     requestItemRemoveQueue.end();
    for (; requestItemRemoveIter != requestItemRemoveEnd;
           requestItemRemoveIter++) {
      rv = (*requestItemRemoveIter)->list->Remove((*requestItemRemoveIter)->item);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }
  // remove the requests from the queue; however, since removing any one item
  // will invalidate the iterators, we first shift all the valid ones to the
  // start of the queue, then remove all the unwanted ones in a batch.  Good
  // thing nsRefPtr has useful assignment.
  // while doing this, we also use this as an opportunity to fix the batch
  // counts.
  { /* scope */
    TransferRequestQueue::iterator
      batchStart, // the start of the current batch
      nextFree,   // the place to insert the next item
      nextItem,   // the next item to examine
      queueEnd = aQueue.end();
    batchStart = nextFree = nextItem = aQueue.begin();
    /* the request queue:
      -------------------------------------------------------------------------
         ^- batch start    ^- next free          ^- next item        queue end-^
     */
    int lastType = 0; /* reserved */
    PRUint32 batchSize = 0;
    for (;; ++nextItem) {
      /* sanity checking; not really needed */
      NS_ASSERTION(find_iterator(batchStart, queueEnd, nextFree) != queueEnd, "invalid iterator state");
      NS_ASSERTION(find_iterator(nextFree, queueEnd, nextItem) != queueEnd, "invalid iterator state");
      NS_ASSERTION(find_iterator(nextItem, queueEnd, queueEnd) == queueEnd, "invalid iterator state");

      bool remove = (nextItem != queueEnd) &&
                      (removalEnd != itemsToRemove.find((*nextItem)->item) ||
                       removalEnd != itemsToRemove.find((*nextItem)->list));

      if (remove) {
        // go to the next item; don't fix up batch counts, so that we end up
        // merging requests
        continue;
      }

      int newType = 0;
      if (nextItem != queueEnd) {
        newType = (*nextItem)->IsCountable() ? (*nextItem)->type : 0;
      } else {
        // at the end of queue; force a type change
        newType = lastType + 1;
      }
      if ((lastType != 0) &&
          (newType != 0) &&
          (lastType != newType))
      {
        // the request type changed; need to fix up the batch counts
        PRUint32 batchIndex = 1;
        for (; batchStart != nextFree; ++batchStart) {
          if ((*batchStart)->IsCountable()) {
            NS_ASSERTION((*batchStart)->type == lastType,
                         "Unexpected item type");
            (*batchStart)->batchIndex = batchIndex;
            (*batchStart)->batchCount = batchSize;
            ++batchIndex;
          }
        }
        NS_ASSERTION(batchIndex == batchSize + 1,
                     "Unexpected ending batch index");
        batchSize = 0;
      }

      if (nextItem == queueEnd) {
        // end of queue
        break;
      }

      if (newType != 0) {
        // this is a countable request, possibly of a different type
        lastType = newType;
        ++batchSize;
      }

      if (nextFree != nextItem) {
        // shift the request, if necessary
        *nextFree = *nextItem;
      }

      ++nextFree;
    }

    LOG(("                        sbBaseDevice::EnsureSpaceForWrite - erasing %u items", std::distance(queueEnd, nextFree)));
    aQueue.erase(nextFree, queueEnd);
  }

  LOG(("                        sbBaseDevice::EnsureSpaceForWrite--\n"));
  return NS_OK;
}

nsresult
sbBaseDevice::WaitForBatchEnd()
{
  nsresult rv;

  // wait for the complete batch to be pushed into the queue
  rv = mBatchEndTimer->InitWithFuncCallback(WaitForBatchEndCallback,
                                            this,
                                            BATCH_TIMEOUT,
                                            nsITimer::TYPE_ONE_SHOT);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

/* static */
void sbBaseDevice::WaitForBatchEndCallback(nsITimer* aTimer,
                                           void* aClosure)
{
  // dispatch callback into non-static method
  sbBaseDevice* self = reinterpret_cast<sbBaseDevice*>(aClosure);
  NS_ASSERTION(self, "self is null");
  self->WaitForBatchEndCallback();
}

void
sbBaseDevice::WaitForBatchEndCallback()
{
  // start processing requests now that the complete batch is available
  ProcessRequest();
}

/* a helper class to proxy sbBaseDevice::Init onto the main thread
 * needed because sbBaseDevice multiply-inherits from nsISupports, so
 * AddRef gets confused
 */
class sbBaseDeviceInitHelper : public nsRunnable
{
public:
  sbBaseDeviceInitHelper(sbBaseDevice* aDevice)
    : mDevice(aDevice) {
      NS_ADDREF(NS_ISUPPORTS_CAST(sbIDevice*, mDevice));
    }

  NS_IMETHOD Run() {
    mDevice->Init();
    return NS_OK;
  }

private:
  ~sbBaseDeviceInitHelper() {
    NS_ISUPPORTS_CAST(sbIDevice*, mDevice)->Release();
  }
  sbBaseDevice* mDevice;
};

nsresult sbBaseDevice::Init()
{
  NS_ASSERTION(NS_IsMainThread(),
               "base device init not on main thread, implement proxying");
  if (!NS_IsMainThread()) {
    // we need to get the weak reference on the main thread because it is not
    // threadsafe, but we only ever use it from the main thread
    nsCOMPtr<nsIRunnable> event = new sbBaseDeviceInitHelper(this);
    return NS_DispatchToMainThread(event, NS_DISPATCH_SYNC);
  }

  nsresult rv;
  // get a weak ref of the device manager
  nsCOMPtr<nsISupportsWeakReference> manager =
    do_GetService("@songbirdnest.com/Songbird/DeviceManager;2", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = manager->GetWeakReference(getter_AddRefs(mParentEventTarget));
  if (NS_FAILED(rv)) {
    mParentEventTarget = nsnull;
    return rv;
  }

  // create a timer used to wait for complete batches in queue
  nsCOMPtr<nsITimer> batchEndTimer = do_CreateInstance(NS_TIMER_CONTRACTID,
                                                       &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = SB_GetProxyForObject(NS_PROXY_TO_MAIN_THREAD,
                            NS_GET_IID(nsITimer),
                            batchEndTimer,
                            nsIProxyObjectManager::INVOKE_SYNC |
                            nsIProxyObjectManager::FORCE_PROXY_CREATION,
                            getter_AddRefs(mBatchEndTimer));
  NS_ENSURE_SUCCESS(rv, rv);

  // Perform derived class intialization
  return InitDevice();
}

nsresult
sbBaseDevice::GetMusicFreeSpace(sbILibrary* aLibrary,
                                PRInt64*    aFreeMusicSpace)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aFreeMusicSpace);

  // Function variables.
  nsresult rv;

  // Get the available music space.
  PRInt64 musicAvailableSpace;
  rv = GetMusicAvailableSpace(aLibrary, &musicAvailableSpace);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the device properties.
  nsCOMPtr<sbIDeviceProperties> baseDeviceProperties;
  nsCOMPtr<nsIPropertyBag2>     deviceProperties;
  rv = GetProperties(getter_AddRefs(baseDeviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = baseDeviceProperties->GetProperties(getter_AddRefs(deviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the music used space.
  PRInt64 musicUsedSpace;
  rv = deviceProperties->GetPropertyAsInt64
         (NS_LITERAL_STRING(SB_DEVICE_PROPERTY_MUSIC_USED_SPACE),
          &musicUsedSpace);
  NS_ENSURE_SUCCESS(rv, rv);

  // Return result.
  if (musicAvailableSpace >= musicUsedSpace)
    *aFreeMusicSpace = musicAvailableSpace - musicUsedSpace;
  else
    *aFreeMusicSpace = 0;

  return NS_OK;
}

nsresult
sbBaseDevice::GetMusicAvailableSpace(sbILibrary* aLibrary,
                                     PRInt64*    aMusicAvailableSpace)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aMusicAvailableSpace);

  // Function variables.
  nsresult rv;

  // Get the device properties.
  nsCOMPtr<sbIDeviceProperties> baseDeviceProperties;
  nsCOMPtr<nsIPropertyBag2>     deviceProperties;
  rv = GetProperties(getter_AddRefs(baseDeviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = baseDeviceProperties->GetProperties(getter_AddRefs(deviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the total capacity.
  PRInt64 capacity;
  rv = deviceProperties->GetPropertyAsInt64
                           (NS_LITERAL_STRING(SB_DEVICE_PROPERTY_CAPACITY),
                            &capacity);
  NS_ENSURE_SUCCESS(rv, rv);

  // Compute the amount of available music space.
  PRInt64 musicAvailableSpace;
  if (mMusicLimitPercent < 100) {
    musicAvailableSpace = (capacity * mMusicLimitPercent) /
                          static_cast<PRInt64>(100);
  } else {
    musicAvailableSpace = capacity;
  }

  // Return results.
  *aMusicAvailableSpace = musicAvailableSpace;

  return NS_OK;
}

//------------------------------------------------------------------------------
//
// Device preference services.
//
//------------------------------------------------------------------------------

NS_IMETHODIMP sbBaseDevice::SetWarningDialogEnabled(const nsAString & aWarning, PRBool aEnabled)
{
  nsresult rv;

  // get the key for this warning
  nsString prefKey(NS_LITERAL_STRING(PREF_WARNING));
  prefKey.Append(aWarning);

  // have a variant to set
  nsCOMPtr<nsIWritableVariant> var =
    do_CreateInstance("@songbirdnest.com/Songbird/Variant;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = var->SetAsBool(aEnabled);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = SetPreference(prefKey, var);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP sbBaseDevice::GetWarningDialogEnabled(const nsAString & aWarning, PRBool *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv;

  // get the key for this warning
  nsString prefKey(NS_LITERAL_STRING(PREF_WARNING));
  prefKey.Append(aWarning);

  nsCOMPtr<nsIVariant> var;
  rv = GetPreference(prefKey, getter_AddRefs(var));
  NS_ENSURE_SUCCESS(rv, rv);

  // does the pref exist?
  PRUint16 dataType;
  rv = var->GetDataType(&dataType);
  NS_ENSURE_SUCCESS(rv, rv);

  if (dataType == nsIDataType::VTYPE_EMPTY ||
      dataType == nsIDataType::VTYPE_VOID)
  {
    // by default warnings are enabled
    *_retval = PR_TRUE;
  } else {
    rv = var->GetAsBool(_retval);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

NS_IMETHODIMP sbBaseDevice::ResetWarningDialogs()
{
  nsresult rv;

  // get the pref branch for this device
  nsCOMPtr<nsIPrefBranch> prefBranch;
  rv = GetPrefBranch(getter_AddRefs(prefBranch));
  NS_ENSURE_SUCCESS(rv, rv);

  // the key for all warnings
  nsString prefKey(NS_LITERAL_STRING(PREF_WARNING));

  // clear the prefs
  rv = prefBranch->DeleteBranch(NS_ConvertUTF16toUTF8(prefKey).get());
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::GetPrefBranch(nsIPrefBranch** aPrefBranch)
{
  NS_ENSURE_ARG_POINTER(aPrefBranch);

  nsresult rv;

  // get the prefs service
  nsCOMPtr<nsIPrefService> prefService =
    do_GetService(NS_PREFSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // If we're not on the main thread proxy the service
  PRBool const isMainThread = NS_IsMainThread();
  if (!isMainThread) {
    nsCOMPtr<nsIPrefService> proxy;
    rv = SB_GetProxyForObject(NS_PROXY_TO_MAIN_THREAD,
                              NS_GET_IID(nsIPrefService),
                              prefService,
                              nsIProxyObjectManager::INVOKE_SYNC |
                              nsIProxyObjectManager::FORCE_PROXY_CREATION,
                              getter_AddRefs(proxy));
    NS_ENSURE_SUCCESS(rv, rv);
    prefService.swap(proxy);
  }

  // get id of this device
  nsID* id;
  rv = GetId(&id);
  NS_ENSURE_SUCCESS(rv, rv);

  // get that as a string
  char idString[NSID_LENGTH];
  id->ToProvidedString(idString);
  NS_Free(id);

  // create the pref key
  nsCString prefKey(PREF_DEVICE_PREFERENCES_BRANCH);
  prefKey.Append(idString);
  prefKey.AppendLiteral(".preferences.");

  // get the pref branch
  nsCOMPtr<nsIPrefBranch> prefBranch;
  rv = prefService->GetBranch(prefKey.get(), getter_AddRefs(prefBranch));
  NS_ENSURE_SUCCESS(rv, rv);

  // If we're not on the main thread proxy the pref branch
  if (!isMainThread) {
    nsCOMPtr<nsIPrefBranch> proxy;
    rv = SB_GetProxyForObject(NS_PROXY_TO_MAIN_THREAD,
                              NS_GET_IID(nsIPrefBranch),
                              prefBranch,
                              nsIProxyObjectManager::INVOKE_SYNC |
                              nsIProxyObjectManager::FORCE_PROXY_CREATION,
                              getter_AddRefs(proxy));
    NS_ENSURE_SUCCESS(rv, rv);
    prefBranch.swap(proxy);
  }

  prefBranch.forget(aPrefBranch);

  return rv;
}

nsresult sbBaseDevice::ApplyPreference(const nsAString& aPrefName,
                                       nsIVariant*      aPrefValue)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aPrefValue);

  // Function variables.
  nsresult rv;

  // Check if it's a library preference.
  PRBool isLibraryPreference = GetIsLibraryPreference(aPrefName);

  // Apply preference.
  if (isLibraryPreference) {
    // Get the library preference info.
    nsCOMPtr<sbIDeviceLibrary> library;
    nsAutoString               libraryPrefBase;
    nsAutoString               libraryPrefName;
    rv = GetPreferenceLibrary(aPrefName,
                              getter_AddRefs(library),
                              libraryPrefBase);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = GetLibraryPreferenceName(aPrefName, libraryPrefBase, libraryPrefName);
    NS_ENSURE_SUCCESS(rv, rv);

    // Apply the library preference.
    rv = ApplyLibraryPreference(library, libraryPrefName, aPrefValue);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

PRBool sbBaseDevice::GetIsLibraryPreference(const nsAString& aPrefName)
{
  return StringBeginsWith(aPrefName,
                          NS_LITERAL_STRING(PREF_DEVICE_LIBRARY_BASE));
}

nsresult sbBaseDevice::GetPreferenceLibrary(const nsAString&   aPrefName,
                                            sbIDeviceLibrary** aLibrary,
                                            nsAString&         aLibraryPrefBase)
{
  // Function variables.
  nsresult rv;

  // Get the device content.
  nsCOMPtr<sbIDeviceContent> content;
  rv = GetContent(getter_AddRefs(content));
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIArray> libraryList;
  rv = content->GetLibraries(getter_AddRefs(libraryList));
  NS_ENSURE_SUCCESS(rv, rv);

  // Search the libraries for a match.  Return results if found.
  PRUint32 libraryCount;
  rv = libraryList->GetLength(&libraryCount);
  NS_ENSURE_SUCCESS(rv, rv);
  for (PRUint32 i = 0; i < libraryCount; i++) {
    // Get the next library.
    nsCOMPtr<sbIDeviceLibrary> library = do_QueryElementAt(libraryList, i, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the library info.
    nsAutoString guid;
    rv = library->GetGuid(guid);
    NS_ENSURE_SUCCESS(rv, rv);

    // Return library if it matches pref.
    nsAutoString libraryPrefBase;
    rv = GetLibraryPreferenceBase(library, libraryPrefBase);
    NS_ENSURE_SUCCESS(rv, rv);
    if (StringBeginsWith(aPrefName, libraryPrefBase)) {
      if (aLibrary)
        library.forget(aLibrary);
      aLibraryPrefBase.Assign(libraryPrefBase);
      return NS_OK;
    }
  }

  // Library not found.
  return NS_ERROR_NOT_AVAILABLE;
}

nsresult sbBaseDevice::GetLibraryPreference(sbIDeviceLibrary* aLibrary,
                                            const nsAString&  aLibraryPrefName,
                                            nsIVariant**      aPrefValue)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aLibrary);

  // Function variables.
  nsresult rv;

  // Get the library preference base.
  nsAutoString libraryPrefBase;
  rv = GetLibraryPreferenceBase(aLibrary, libraryPrefBase);
  NS_ENSURE_SUCCESS(rv, rv);

  return GetLibraryPreference(libraryPrefBase, aLibraryPrefName, aPrefValue);
}

nsresult sbBaseDevice::GetLibraryPreference(const nsAString& aLibraryPrefBase,
                                            const nsAString& aLibraryPrefName,
                                            nsIVariant**     aPrefValue)
{
  // Get the full preference name.
  nsAutoString prefName(aLibraryPrefBase);
  prefName.Append(aLibraryPrefName);

  return GetPreference(prefName, aPrefValue);
}

nsresult sbBaseDevice::ApplyLibraryPreference
                         (sbIDeviceLibrary* aLibrary,
                          const nsAString&  aLibraryPrefName,
                          nsIVariant*       aPrefValue)
{
  nsresult rv;

  // Operate under the preference lock.
  nsAutoLock preferenceLock(mPreferenceLock);

  // Get the library pref base.
  nsAutoString prefBase;
  rv = GetLibraryPreferenceBase(aLibrary, prefBase);
  NS_ENSURE_SUCCESS(rv, rv);

  // If no preference name is specified, read and apply all library preferences.
  PRBool applyAll = PR_FALSE;
  if (aLibraryPrefName.IsEmpty())
    applyAll = PR_TRUE;

  // Apply music limit preference.
  if (applyAll || aLibraryPrefName.EqualsLiteral("music_limit_percent")) {
    // Get the preference value.
    nsCOMPtr<nsIVariant> prefValue = aPrefValue;
    if (applyAll || !prefValue) {
      rv = GetLibraryPreference(prefBase,
                                NS_LITERAL_STRING("music_limit_percent"),
                                getter_AddRefs(prefValue));
      if (NS_FAILED(rv))
        prefValue = nsnull;
    }

    // Apply the preference value.
    if (prefValue) {
      PRUint32 musicLimitPercent;
      rv = prefValue->GetAsUint32(&musicLimitPercent);
      if (NS_SUCCEEDED(rv))
        mMusicLimitPercent = musicLimitPercent;
    }
  }

  return NS_OK;
}

nsresult sbBaseDevice::GetLibraryPreferenceName
                         (const nsAString& aPrefName,
                          nsAString&       aLibraryPrefName)
{
  nsresult rv;

  // Get the preference library preference base.
  nsAutoString               libraryPrefBase;
  rv = GetPreferenceLibrary(aPrefName, nsnull, libraryPrefBase);
  NS_ENSURE_SUCCESS(rv, rv);

  return GetLibraryPreferenceName(aPrefName, libraryPrefBase, aLibraryPrefName);
}

nsresult sbBaseDevice::GetLibraryPreferenceName
                         (const nsAString&  aPrefName,
                          const nsAString&  aLibraryPrefBase,
                          nsAString&        aLibraryPrefName)
{
  // Validate pref name.
  NS_ENSURE_TRUE(StringBeginsWith(aPrefName, aLibraryPrefBase),
                 NS_ERROR_ILLEGAL_VALUE);

  // Extract the library preference name.
  aLibraryPrefName.Assign(Substring(aPrefName, aLibraryPrefBase.Length()));

  return NS_OK;
}

nsresult sbBaseDevice::GetLibraryPreferenceBase(sbIDeviceLibrary* aLibrary,
                                                nsAString&        aPrefBase)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aLibrary);

  // Function variables.
  nsresult rv;

  // Get the library info.
  nsAutoString guid;
  rv = aLibrary->GetGuid(guid);
  NS_ENSURE_SUCCESS(rv, rv);

  // Produce the library preference base.
  aPrefBase.Assign(NS_LITERAL_STRING(PREF_DEVICE_LIBRARY_BASE));
  aPrefBase.Append(guid);
  aPrefBase.AppendLiteral(".");

  return NS_OK;
}

//------------------------------------------------------------------------------
//
// Device sync services.
//
//------------------------------------------------------------------------------

nsresult sbBaseDevice::SyncCheckLinkedPartner(PRBool  aRequestPartnerChange,
                                              PRBool* aIsLinkedLocally)
{
  NS_ENSURE_ARG_POINTER(aIsLinkedLocally);

  nsresult rv;

  // Get the device sync partner ID and determine if the device is linked to a
  // sync partner.
  PRBool               deviceIsLinked;
  nsCOMPtr<nsIVariant> deviceSyncPartnerIDVariant;
  nsAutoString         deviceSyncPartnerID;
  rv = GetPreference(NS_LITERAL_STRING("SyncPartner"),
                     getter_AddRefs(deviceSyncPartnerIDVariant));
  if (NS_SUCCEEDED(rv)) {
    rv = deviceSyncPartnerIDVariant->GetAsAString(deviceSyncPartnerID);
    NS_ENSURE_SUCCESS(rv, rv);
    deviceIsLinked = PR_TRUE;
  } else {
    deviceIsLinked = PR_FALSE;
  }

  // Get the local sync partner ID.
  nsAutoString localSyncPartnerID;
  rv = GetMainLibraryId(localSyncPartnerID);
  NS_ENSURE_SUCCESS(rv, rv);

  // Check if device is linked to local sync partner.
  PRBool isLinkedLocally = PR_FALSE;
  if (deviceIsLinked)
    isLinkedLocally = deviceSyncPartnerID.Equals(localSyncPartnerID);

  // If device is not linked locally, request that its sync partner be changed.
  if (!isLinkedLocally && aRequestPartnerChange) {
    // Request that the device sync partner be changed.
    PRBool partnerChangeGranted;
    rv = SyncRequestPartnerChange(&partnerChangeGranted);
    NS_ENSURE_SUCCESS(rv, rv);

    // Change the sync partner if the request was granted.
    if (partnerChangeGranted) {
      rv = SetPreference(NS_LITERAL_STRING("SyncPartner"),
                         sbNewVariant(localSyncPartnerID));
      NS_ENSURE_SUCCESS(rv, rv);
      isLinkedLocally = PR_TRUE;
    }
  }

  // Return results.
  *aIsLinkedLocally = isLinkedLocally;

  return NS_OK;
}

nsresult sbBaseDevice::SyncRequestPartnerChange(PRBool* aPartnerChangeGranted)
{
  NS_ENSURE_ARG_POINTER(aPartnerChangeGranted);

  nsresult rv;

  // Get the device name.
  nsString deviceName;
  rv = GetName(deviceName);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the main library name.
  nsCOMPtr<sbILibrary> mainLibrary;
  nsString             libraryName;
  rv = GetMainLibrary(getter_AddRefs(mainLibrary));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = mainLibrary->GetName(libraryName);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get a prompter that waits for a window.
  nsCOMPtr<sbIPrompter> prompter =
                          do_CreateInstance(SONGBIRD_PROMPTER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = prompter->SetWaitForWindow(PR_TRUE);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the localized string bundle.
  sbStringBundle bundle;

  // Ensure that the library name is not empty.
  if (libraryName.IsEmpty()) {
    libraryName = bundle.Get("servicesource.library");
  }

  // Get the prompt title.
  nsAString const& title = bundle.Get("device.dialog.sync_confirmation.title");

  // Get the prompt message.
  nsTArray<nsString> formatParams;
  formatParams.AppendElement(deviceName);
  formatParams.AppendElement(libraryName);
  nsAString const& message =
                     bundle.Format("device.dialog.sync_confirmation.msg",
                                   formatParams);

  // Configure the buttons.
  PRUint32 buttonFlags = 0;

  // Configure the no button as button 1.
  nsAString const& noButton =
                     bundle.Get("device.dialog.sync_confirmation.no_button");
  buttonFlags += (nsIPromptService::BUTTON_POS_1 *
                  nsIPromptService::BUTTON_TITLE_IS_STRING);

  // Configure the sync button as button 0.
  nsAString const& syncButton =
                     bundle.Get("device.dialog.sync_confirmation.sync_button");
  buttonFlags += (nsIPromptService::BUTTON_POS_0 *
                  nsIPromptService::BUTTON_TITLE_IS_STRING) +
                 nsIPromptService::BUTTON_POS_0_DEFAULT;
  PRInt32 grantPartnerChangeIndex = 0;

  // XXX lone> see mozbug 345067, there is no way to tell the prompt service
  // what code to return when the titlebar's X close button is clicked, it is
  // always 1, so we have to make the No button the second button.

  // Query the user to determine whether the device sync partner should be
  // changed to the local partner.
  PRInt32 buttonPressed;
  rv = prompter->ConfirmEx(nsnull,
                           title.BeginReading(),
                           message.BeginReading(),
                           buttonFlags,
                           syncButton.BeginReading(),
                           noButton.BeginReading(),
                           nsnull,                      // No button 2.
                           nsnull,                      // No check message.
                           nsnull,                      // No check result.
                           &buttonPressed);
  NS_ENSURE_SUCCESS(rv, rv);

  // Check if partner change request was granted.
  if (buttonPressed == grantPartnerChangeIndex)
    *aPartnerChangeGranted = PR_TRUE;
  else
    *aPartnerChangeGranted = PR_FALSE;

  return NS_OK;
}

nsresult
sbBaseDevice::HandleSyncRequest(TransferRequest* aRequest)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aRequest);

  // Function variables.
  nsresult rv;

  // Cancel operation if device is not linked to the local sync partner.
  PRBool isLinkedLocally;
  rv = SyncCheckLinkedPartner(PR_TRUE, &isLinkedLocally);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!isLinkedLocally) {
    rv = SetState(STATE_CANCEL);
    NS_ENSURE_SUCCESS(rv, rv);
    return NS_OK;
  }

  // Ensure enough space is available for operation.  Cancel operation if not.
  PRBool abort;
  rv = EnsureSpaceForSync(aRequest, &abort);
  NS_ENSURE_SUCCESS(rv, rv);
  if (abort) {
    rv = SetState(STATE_CANCEL);
    NS_ENSURE_SUCCESS(rv, rv);
    return NS_OK;
  }

  // Produce the sync change set.
  nsCOMPtr<sbILibraryChangeset> changeset;
  rv = SyncProduceChangeset(aRequest, getter_AddRefs(changeset));
  NS_ENSURE_SUCCESS(rv, rv);

  if (IsRequestAbortedOrDeviceDisconnected()) {
    return SB_ERROR_REQUEST_ABORTED;
  }

  // Apply changes to the destination library.
  nsCOMPtr<sbIDeviceLibrary> dstLib = do_QueryInterface(aRequest->list, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = SyncApplyChanges(dstLib, changeset);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbBaseDevice::EnsureSpaceForSync(TransferRequest* aRequest,
                                 PRBool*          aAbort)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aRequest);
  NS_ENSURE_ARG_POINTER(aAbort);

  // Function variables.
  PRBool   success;
  nsresult rv;

  // Get the sync source and destination libraries.
  nsCOMPtr<sbILibrary> srcLib = do_QueryInterface(aRequest->item, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<sbIDeviceLibrary> dstLib = do_QueryInterface(aRequest->list, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Allocate a list of sync items and a hash table of their sizes.
  nsCOMArray<sbIMediaItem>                     syncItemList;
  nsDataHashtable<nsISupportsHashKey, PRInt64> syncItemSizeMap;
  success = syncItemSizeMap.Init();
  NS_ENSURE_TRUE(success, NS_ERROR_OUT_OF_MEMORY);

  // Get the sync item sizes and total size.
  PRInt64 totalSyncSize;
  rv = SyncGetSyncItemSizes(srcLib,
                            dstLib,
                            syncItemList,
                            syncItemSizeMap,
                            &totalSyncSize);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the space available for syncing.
  PRInt64 availableSpace;
  rv = SyncGetSyncAvailableSpace(dstLib, &availableSpace);
  NS_ENSURE_SUCCESS(rv, rv);

  // If not enough space is available, ask the user what action to take.  If the
  // user does not abort the operation, create and sync to a sync media list
  // that will fit in the available space.  Otherwise, set the management type
  // to manual and return with abort.
  if (availableSpace < totalSyncSize) {
    // Ask the user what action to take.
    PRBool abort;
    rv = sbDeviceUtils::QueryUserSpaceExceeded(this,
                                               dstLib,
                                               totalSyncSize,
                                               availableSpace,
                                               &abort);
    NS_ENSURE_SUCCESS(rv, rv);
    if (abort) {
      rv = dstLib->SetMgmtType(sbIDeviceLibrary::MGMT_TYPE_MANUAL);
      NS_ENSURE_SUCCESS(rv, rv);
      *aAbort = PR_TRUE;
      return NS_OK;
    }

    // Create a sync media list and sync to it.
    rv = SyncCreateAndSyncToList(srcLib,
                                 dstLib,
                                 syncItemList,
                                 syncItemSizeMap,
                                 availableSpace);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Don't abort.
  *aAbort = PR_FALSE;

  return NS_OK;
}

nsresult
sbBaseDevice::SyncCreateAndSyncToList
  (sbILibrary*                                   aSrcLib,
   sbIDeviceLibrary*                             aDstLib,
   nsCOMArray<sbIMediaItem>&                     aSyncItemList,
   nsDataHashtable<nsISupportsHashKey, PRInt64>& aSyncItemSizeMap,
   PRInt64                                       aAvailableSpace)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aSrcLib);
  NS_ENSURE_ARG_POINTER(aDstLib);

  // Function variables.
  nsresult rv;

  // Set to sync to an empty list of playlists to prevent syncing while the sync
  // playlist is created.
  nsCOMPtr<nsIMutableArray> emptySyncPlaylistList =
    do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aDstLib->SetSyncPlaylistList(emptySyncPlaylistList);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aDstLib->SetMgmtType(sbIDeviceLibrary::MGMT_TYPE_SYNC_PLAYLISTS);
  NS_ENSURE_SUCCESS(rv, rv);

  // Create a shuffled sync item list that will fit in the available space.
  nsCOMPtr<nsIArray> syncItemList;
  rv = SyncShuffleSyncItemList(aSyncItemList,
                               aSyncItemSizeMap,
                               aAvailableSpace,
                               getter_AddRefs(syncItemList));
  NS_ENSURE_SUCCESS(rv, rv);

  // Create a new source sync media list.
  nsCOMPtr<sbIMediaList> syncMediaList;
  rv = SyncCreateSyncMediaList(aSrcLib,
                               syncItemList,
                               getter_AddRefs(syncMediaList));
  NS_ENSURE_SUCCESS(rv, rv);

  // Sync to the sync media list.
  rv = SyncToMediaList(aDstLib, syncMediaList);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncShuffleSyncItemList
  (nsCOMArray<sbIMediaItem>&                     aSyncItemList,
   nsDataHashtable<nsISupportsHashKey, PRInt64>& aSyncItemSizeMap,
   PRInt64                                       aAvailableSpace,
   nsIArray**                                    aShuffleSyncItemList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aShuffleSyncItemList);

  // Function variables.
  PRBool   success;
  nsresult rv;

  // Get the sync item list info.
  PRInt32 itemCount = aSyncItemList.Count();

  // Seed the random number generator for shuffling.
  srand((unsigned int) (PR_Now() & 0xFFFFFFFF));

  // Copy the sync item list to a vector and shuffle it.
  std::vector<sbIMediaItem*> randomItemList;
  for (PRInt32 i = 0; i < itemCount; i++) {
    randomItemList.push_back(aSyncItemList[i]);
  }
  std::random_shuffle(randomItemList.begin(), randomItemList.end());

  // Create a shuffled sync item list that will fill the available space.
  nsCOMPtr<nsIMutableArray> shuffleSyncItemList =
    do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Add sync items from the shuffled list to fill the available space,
  // reserving some margin for error.
  PRInt64 remainingSpace = aAvailableSpace;
  remainingSpace -= (aAvailableSpace * SYNC_PLAYLIST_MARGIN_PCT) / 100;
  for (PRInt32 i = 0; i < itemCount; i++) {
    // Get the next item and its info.
    nsCOMPtr<sbIMediaItem> syncItem = randomItemList[i];
    PRInt64                itemSize;
    success = aSyncItemSizeMap.Get(syncItem, &itemSize);
    NS_ENSURE_TRUE(success, NS_ERROR_FAILURE);

    // Add item if it fits in remaining space.
    if (remainingSpace >= itemSize) {
      rv = shuffleSyncItemList->AppendElement(syncItem, PR_FALSE);
      NS_ENSURE_SUCCESS(rv, rv);
      remainingSpace -= itemSize;
    }
  }

  // Return results.
  NS_ADDREF(*aShuffleSyncItemList = shuffleSyncItemList);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncCreateSyncMediaList(sbILibrary*    aSrcLib,
                                      nsIArray*      aSyncItemList,
                                      sbIMediaList** aSyncMediaList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aSrcLib);
  NS_ENSURE_ARG_POINTER(aSyncItemList);
  NS_ENSURE_ARG_POINTER(aSyncMediaList);

  // Function variables.
  nsresult rv;

  // Create a new source sync media list.
  nsCOMPtr<sbIMediaList> syncMediaList;
  rv = aSrcLib->CreateMediaList(NS_LITERAL_STRING("simple"),
                                nsnull,
                                getter_AddRefs(syncMediaList));
  NS_ENSURE_SUCCESS(rv, rv);

  // Add the sync item list to the sync media list.
  nsCOMPtr<nsISimpleEnumerator> syncItemListEnum;
  rv = aSyncItemList->Enumerate(getter_AddRefs(syncItemListEnum));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = syncMediaList->AddSome(syncItemListEnum);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the device name.
  nsAutoString deviceName;
  rv = GetName(deviceName);
  NS_ENSURE_SUCCESS(rv, rv);

  // Produce the sync media list name.
  nsString listName = SBLocalizedString
                        ("device.error.not_enough_freespace.playlist_name");

  // Set the sync media list name.
  rv = syncMediaList->SetName(listName);
  NS_ENSURE_SUCCESS(rv, rv);

  // Return results.
  *aSyncMediaList = nsnull;
  syncMediaList.swap(*aSyncMediaList);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncToMediaList(sbIDeviceLibrary* aDstLib,
                              sbIMediaList*     aMediaList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aDstLib);
  NS_ENSURE_ARG_POINTER(aMediaList);

  // Function variables.
  nsresult rv;

  // Create a sync playlist list array with the sync media list.
  nsCOMPtr<nsIMutableArray> syncPlaylistList =
    do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = syncPlaylistList->AppendElement(aMediaList, PR_FALSE);
  NS_ENSURE_SUCCESS(rv, rv);

  // Set to sync to the sync media list.
  rv = aDstLib->SetSyncPlaylistList(syncPlaylistList);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aDstLib->SetMgmtType(sbIDeviceLibrary::MGMT_TYPE_SYNC_PLAYLISTS);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncGetSyncItemSizes
  (sbILibrary*                                   aSrcLib,
   sbIDeviceLibrary*                             aDstLib,
   nsCOMArray<sbIMediaItem>&                     aSyncItemList,
   nsDataHashtable<nsISupportsHashKey, PRInt64>& aSyncItemSizeMap,
   PRInt64*                                      aTotalSyncSize)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aSrcLib);
  NS_ENSURE_ARG_POINTER(aDstLib);
  NS_ENSURE_ARG_POINTER(aTotalSyncSize);

  // Function variables.
  nsresult rv;

  // Get the list of sync media lists.
  nsCOMPtr<nsIArray> syncList;
  rv = SyncGetSyncList(aSrcLib, aDstLib, getter_AddRefs(syncList));
  NS_ENSURE_SUCCESS(rv, rv);

  // Fill in the sync item size table and calculate the total sync size.
  PRInt64 totalSyncSize = 0;
  PRUint32 syncListLength;
  rv = syncList->GetLength(&syncListLength);
  NS_ENSURE_SUCCESS(rv, rv);
  for (PRUint32 i = 0; i < syncListLength; i++) {
    // Get the next sync media list.
    nsCOMPtr<sbIMediaList> syncML = do_QueryElementAt(syncList, i, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the sizes of the sync media items.
    rv = SyncGetSyncItemSizes(syncML,
                              aSyncItemList,
                              aSyncItemSizeMap,
                              &totalSyncSize);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Return results.
  *aTotalSyncSize = totalSyncSize;

  return NS_OK;
}

nsresult
sbBaseDevice::SyncGetSyncItemSizes
  (sbIMediaList*                                 aSyncML,
   nsCOMArray<sbIMediaItem>&                     aSyncItemList,
   nsDataHashtable<nsISupportsHashKey, PRInt64>& aSyncItemSizeMap,
   PRInt64*                                      aTotalSyncSize)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aSyncML);
  NS_ENSURE_ARG_POINTER(aTotalSyncSize);

  // Function variables.
  PRBool   success;
  nsresult rv;

  // Accumulate the sizes of all sync items.  Use GetItemByIndex like the
  // diffing service does.
  PRInt64  totalSyncSize = *aTotalSyncSize;
  PRUint32 itemCount;
  rv = aSyncML->GetLength(&itemCount);
  NS_ENSURE_SUCCESS(rv, rv);
  for (PRUint32 i = 0; i < itemCount; i++) {
    // Get the sync item.
    nsCOMPtr<sbIMediaItem> mediaItem;
    rv = aSyncML->GetItemByIndex(i, getter_AddRefs(mediaItem));
    NS_ENSURE_SUCCESS(rv, rv);

    // Ignore media lists.
    nsCOMPtr<sbIMediaList> mediaList = do_QueryInterface(mediaItem, &rv);
    if (NS_SUCCEEDED(rv))
      continue;

    // Do nothing more if item is already in list.
    if (aSyncItemSizeMap.Get(mediaItem, nsnull))
      continue;

    // Get the item size adding in the per track overhead.  Assume a length of
    // 0 on error.
    PRInt64 contentLength;
    rv = sbLibraryUtils::GetContentLength(mediaItem, &contentLength);
    if (NS_FAILED(rv))
      contentLength = 0;
    contentLength += mPerTrackOverhead;

    // Add item.
    success = aSyncItemList.AppendObject(mediaItem);
    NS_ENSURE_TRUE(success, NS_ERROR_OUT_OF_MEMORY);
    success = aSyncItemSizeMap.Put(mediaItem, contentLength);
    NS_ENSURE_TRUE(success, NS_ERROR_OUT_OF_MEMORY);
    totalSyncSize += contentLength;
  }

  // Return results.
  *aTotalSyncSize = totalSyncSize;

  return NS_OK;
}

nsresult
sbBaseDevice::SyncGetSyncList(sbILibrary*       aSrcLib,
                              sbIDeviceLibrary* aDstLib,
                              nsIArray**        aSyncList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aSrcLib);
  NS_ENSURE_ARG_POINTER(aDstLib);
  NS_ENSURE_ARG_POINTER(aSyncList);

  // Function variables.
  nsresult rv;

  // Get the list of sync media lists.
  nsCOMPtr<nsIArray> syncList;
  PRUint32           syncMode;
  rv = aDstLib->GetMgmtType(&syncMode);
  NS_ENSURE_SUCCESS(rv, rv);
  switch (syncMode)
  {
    case sbIDeviceLibrary::MGMT_TYPE_SYNC_ALL:
    {
      // Create a sync all array containing the entire source library.
      nsCOMPtr<nsIMutableArray>
        syncAllList =
          do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1",
                            &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      nsCOMPtr<sbIMediaList> srcLibML = do_QueryInterface(aSrcLib, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      rv = syncAllList->AppendElement(srcLibML, PR_FALSE);
      NS_ENSURE_SUCCESS(rv, rv);

      // Set the sync list.
      syncList = do_QueryInterface(syncAllList, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
    } break;

    case sbIDeviceLibrary::MGMT_TYPE_SYNC_PLAYLISTS:
      rv = aDstLib->GetSyncPlaylistList(getter_AddRefs(syncList));
      NS_ENSURE_SUCCESS(rv, rv);
      break;

    default:
      NS_ENSURE_SUCCESS(NS_ERROR_ILLEGAL_VALUE, NS_ERROR_ILLEGAL_VALUE);
      break;
  }

  // Return results.
  syncList.forget(aSyncList);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncGetSyncAvailableSpace(sbILibrary* aLibrary,
                                        PRInt64*    aAvailableSpace)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aLibrary);
  NS_ENSURE_ARG_POINTER(aAvailableSpace);

  // Function variables.
  nsresult rv;

  // Get the device properties.
  nsCOMPtr<sbIDeviceProperties> baseDeviceProperties;
  nsCOMPtr<nsIPropertyBag2>     deviceProperties;
  rv = GetProperties(getter_AddRefs(baseDeviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = baseDeviceProperties->GetProperties(getter_AddRefs(deviceProperties));
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the free space and the music used space.
  PRInt64 freeSpace;
  PRInt64 musicUsedSpace;
  rv = deviceProperties->GetPropertyAsInt64
                           (NS_LITERAL_STRING(SB_DEVICE_PROPERTY_FREE_SPACE),
                            &freeSpace);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = deviceProperties->GetPropertyAsInt64
         (NS_LITERAL_STRING(SB_DEVICE_PROPERTY_MUSIC_USED_SPACE),
          &musicUsedSpace);
  NS_ENSURE_SUCCESS(rv, rv);

  // Add track overhead to the music used space.
  PRUint32 trackCount;
  rv = aLibrary->GetLength(&trackCount);
  NS_ENSURE_SUCCESS(rv, rv);
  musicUsedSpace += trackCount * mPerTrackOverhead;

  // Determine the total available space for syncing as the free space plus the
  // space used for music.
  PRInt64 availableSpace = freeSpace + musicUsedSpace;

  // Apply limit to the total space available for music.
  PRInt64 musicAvailableSpace;
  rv = GetMusicAvailableSpace(aLibrary, &musicAvailableSpace);
  NS_ENSURE_SUCCESS(rv, rv);
  if (availableSpace >= musicAvailableSpace)
    availableSpace = musicAvailableSpace;

  // Return results.
  *aAvailableSpace = availableSpace;

  return NS_OK;
}

nsresult
sbBaseDevice::SyncProduceChangeset(TransferRequest*      aRequest,
                                   sbILibraryChangeset** aChangeset)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aRequest);
  NS_ENSURE_ARG_POINTER(aChangeset);

  // Function variables.
  nsresult rv;

  // Get the sync source and destination libraries.
  nsCOMPtr<sbILibrary> srcLib = do_QueryInterface(aRequest->item, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<sbIDeviceLibrary> dstLib = do_QueryInterface(aRequest->list, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Set up to force a diff on all media lists so that all source and
  // destination media lists can be compared with each other.
  rv = SyncForceDiffMediaLists(dstLib);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the list of sync media lists.
  nsCOMPtr<nsIArray> syncList;
  PRUint32           syncMode;
  rv = dstLib->GetMgmtType(&syncMode);
  NS_ENSURE_SUCCESS(rv, rv);
  switch (syncMode)
  {
    case sbIDeviceLibrary::MGMT_TYPE_SYNC_ALL:
    {
      // Create a sync all array containing the entire source library.
      nsCOMPtr<nsIMutableArray>
        syncAllList =
          do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1",
                            &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      rv = syncAllList->AppendElement(srcLib, PR_FALSE);
      NS_ENSURE_SUCCESS(rv, rv);

      // Set the sync list.
      syncList = do_QueryInterface(syncAllList, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
    } break;

    case sbIDeviceLibrary::MGMT_TYPE_SYNC_PLAYLISTS:
      rv = dstLib->GetSyncPlaylistList(getter_AddRefs(syncList));
      NS_ENSURE_SUCCESS(rv, rv);
      break;

    default:
      NS_ENSURE_SUCCESS(NS_ERROR_ILLEGAL_VALUE, NS_ERROR_ILLEGAL_VALUE);
      break;
  }

  // Get the diffing service.
  nsCOMPtr<sbILibraryDiffingService>
    diffService = do_GetService(SB_LOCALDATABASE_DIFFINGSERVICE_CONTRACTID,
                                &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Produce the sync changeset.
  nsCOMPtr<sbILibraryChangeset> changeset;
  rv = diffService->CreateMultiChangeset(syncList,
                                         dstLib,
                                         getter_AddRefs(changeset));
  NS_ENSURE_SUCCESS(rv, rv);

  // Return results.
  NS_ADDREF(*aChangeset = changeset);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncApplyChanges(sbIDeviceLibrary*    aDstLibrary,
                               sbILibraryChangeset* aChangeset)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aDstLibrary);
  NS_ENSURE_ARG_POINTER(aChangeset);

  // Function variables.
  PRBool   success;
  nsresult rv;

  // Create some change list arrays.
  nsCOMArray<sbIMediaList>     addMediaListList;
  nsCOMArray<sbILibraryChange> mediaListChangeList;
  nsCOMPtr<nsIMutableArray>    addItemList =
    do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIMutableArray>    deleteItemList =
    do_CreateInstance("@songbirdnest.com/moz/xpcom/threadsafe-array;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the list of all changes.
  nsCOMPtr<nsIArray> changeList;
  PRUint32           changeCount;
  rv = aChangeset->GetChanges(getter_AddRefs(changeList));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = changeList->GetLength(&changeCount);
  NS_ENSURE_SUCCESS(rv, rv);

  // Group changes for later processing but apply property updates immediately.
  for (PRUint32 i = 0; i < changeCount; i++) {
    if (IsRequestAbortedOrDeviceDisconnected()) {
      return SB_ERROR_REQUEST_ABORTED;
    }
    // Get the next change.
    nsCOMPtr<sbILibraryChange> change = do_QueryElementAt(changeList, i, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Put the change into the appropriate list.
    PRUint32 operation;
    rv = change->GetOperation(&operation);
    NS_ENSURE_SUCCESS(rv, rv);
    switch (operation)
    {
      case sbIChangeOperation::DELETED:
        {
          // Add the destination item to the delete list.
          nsCOMPtr<sbIMediaItem> mediaItem;
          rv = change->GetDestinationItem(getter_AddRefs(mediaItem));
          NS_ENSURE_SUCCESS(rv, rv);
          rv = deleteItemList->AppendElement(mediaItem, PR_FALSE);
          NS_ENSURE_SUCCESS(rv, rv);
        } break;

      case sbIChangeOperation::ADDED:
        {
          // Get the source item to add.
          nsCOMPtr<sbIMediaItem> mediaItem;
          rv = change->GetSourceItem(getter_AddRefs(mediaItem));
          NS_ENSURE_SUCCESS(rv, rv);

          // Add item to add media list list or add item list.
          PRBool itemIsList;
          rv = change->GetItemIsList(&itemIsList);
          NS_ENSURE_SUCCESS(rv, rv);
          if (itemIsList) {
            nsCOMPtr<sbIMediaList> mediaList = do_QueryInterface(mediaItem,
                                                                 &rv);
            NS_ENSURE_SUCCESS(rv, rv);
            rv = addMediaListList.AppendObject(mediaList);
            NS_ENSURE_SUCCESS(rv, rv);
          } else {
            rv = addItemList->AppendElement(mediaItem, PR_FALSE);
            NS_ENSURE_SUCCESS(rv, rv);
          }
        } break;

      case sbIChangeOperation::MODIFIED:
        {
          // If the change is to a media list, add it to the media list change
          // list.
          PRBool itemIsList;
          rv = change->GetItemIsList(&itemIsList);
          NS_ENSURE_SUCCESS(rv, rv);
          if (itemIsList) {
            success = mediaListChangeList.AppendObject(change);
            NS_ENSURE_SUCCESS(success, NS_ERROR_FAILURE);
          }

          // Update the item properties.
          rv = SyncUpdateProperties(change);
          NS_ENSURE_SUCCESS(rv, rv);
        } break;

      default:
        break;
    }
  }

  if (IsRequestAbortedOrDeviceDisconnected()) {
    return SB_ERROR_REQUEST_ABORTED;
  }

  // Delete items.
  nsCOMPtr<nsISimpleEnumerator> deleteItemEnum;
  rv = deleteItemList->Enumerate(getter_AddRefs(deleteItemEnum));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aDstLibrary->RemoveSome(deleteItemEnum);
  NS_ENSURE_SUCCESS(rv, rv);

  if (IsRequestAbortedOrDeviceDisconnected()) {
    return SB_ERROR_REQUEST_ABORTED;
  }

  // Add items.
  nsCOMPtr<nsISimpleEnumerator> addItemEnum;
  rv = addItemList->Enumerate(getter_AddRefs(addItemEnum));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aDstLibrary->AddSome(addItemEnum);
  NS_ENSURE_SUCCESS(rv, rv);

  if (IsRequestAbortedOrDeviceDisconnected()) {
    rv = sbDeviceUtils::DeleteByProperty(aDstLibrary,
                                         NS_LITERAL_STRING(SB_PROPERTY_HIDDEN),
                                         NS_LITERAL_STRING("1"));
    NS_ASSERTION(NS_SUCCEEDED(rv), "Failed to remove partial added items");
    return SB_ERROR_REQUEST_ABORTED;
  }

  // Add media lists.
  PRInt32 count = addMediaListList.Count();
  for (PRInt32 i = 0; i < count; i++) {
    if (IsRequestAbortedOrDeviceDisconnected()) {
      return SB_ERROR_REQUEST_ABORTED;
    }
    rv = SyncAddMediaList(aDstLibrary, addMediaListList[i]);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Sync the media lists.
  rv = SyncMediaLists(mediaListChangeList);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncAddMediaList(sbIDeviceLibrary* aDstLibrary,
                               sbIMediaList*     aMediaList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aDstLibrary);
  NS_ENSURE_ARG_POINTER(aMediaList);

  // Function variables.
  nsresult rv;

  // Don't sync media list if it shouldn't be synced.
  PRBool shouldSync = PR_FALSE;
  rv = ShouldSyncMediaList(aMediaList, &shouldSync);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!shouldSync)
    return NS_OK;

  // Create a main thread destination library proxy to add the media lists.
  // This ensures that the library notification for the added media list is
  // delivered and processed before media list items are added.  This ensures
  // that a device listener is added for the added media list in time to get
  // notifications for the added media list items.
  nsCOMPtr<sbIDeviceLibrary> proxyDstLibrary;
  rv = SB_GetProxyForObject(NS_PROXY_TO_MAIN_THREAD,
                            NS_GET_IID(sbIDeviceLibrary),
                            aDstLibrary,
                            nsIProxyObjectManager::INVOKE_SYNC |
                            nsIProxyObjectManager::FORCE_PROXY_CREATION,
                            getter_AddRefs(proxyDstLibrary));
  NS_ENSURE_SUCCESS(rv, rv);

  // Copy the media list.  Don't use the add method because it disables
  // notifications until after the media list and its items are added.
  nsCOMPtr<sbIMediaList> mediaList;
  rv = proxyDstLibrary->CopyMediaList(NS_LITERAL_STRING("simple"),
                                      aMediaList,
                                      getter_AddRefs(mediaList));
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbBaseDevice::SyncMediaLists(nsCOMArray<sbILibraryChange>& aMediaListChangeList)
{
  nsresult rv;

  // Just replace the destination media lists with the source media lists.
  // TODO: just apply changes between the source and destination lists.

  // Sync each media list.
  PRInt32 count = aMediaListChangeList.Count();
  for (PRInt32 i = 0; i < count; i++) {
    // Get the media list change.
    nsCOMPtr<sbILibraryChange> change = aMediaListChangeList[i];

    // Get the destination media list item and library.
    nsCOMPtr<sbIMediaItem>     dstMediaListItem;
    nsCOMPtr<sbILibrary>       dstLibrary;
    nsCOMPtr<sbIDeviceLibrary> dstDeviceLibrary;
    rv = change->GetDestinationItem(getter_AddRefs(dstMediaListItem));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = dstMediaListItem->GetLibrary(getter_AddRefs(dstLibrary));
    NS_ENSURE_SUCCESS(rv, rv);
    dstDeviceLibrary = do_QueryInterface(dstLibrary, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Remove the destination media list item.
    rv = dstLibrary->Remove(dstMediaListItem);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the source media list.
    nsCOMPtr<sbIMediaItem> srcMediaListItem;
    nsCOMPtr<sbIMediaList> srcMediaList;
    rv = change->GetSourceItem(getter_AddRefs(srcMediaListItem));
    NS_ENSURE_SUCCESS(rv, rv);
    srcMediaList = do_QueryInterface(srcMediaListItem, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Add the source media list to the destination library.
    rv = SyncAddMediaList(dstDeviceLibrary, srcMediaList);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult
sbBaseDevice::SyncUpdateProperties(sbILibraryChange* aChange)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aChange);

  // Function variables.
  nsresult rv;

  // Get the item to update.
  nsCOMPtr<sbIMediaItem> mediaItem;
  rv = aChange->GetDestinationItem(getter_AddRefs(mediaItem));
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the list of properties to update.
  nsCOMPtr<nsIArray> propertyList;
  PRUint32           propertyCount;
  rv = aChange->GetProperties(getter_AddRefs(propertyList));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = propertyList->GetLength(&propertyCount);
  NS_ENSURE_SUCCESS(rv, rv);

  // Update properties.
  for (PRUint32 i = 0; i < propertyCount; i++) {
    // Get the next property to update.
    nsCOMPtr<sbIPropertyChange>
      property = do_QueryElementAt(propertyList, i, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the property info.
    nsAutoString propertyID;
    nsAutoString propertyValue;
    nsAutoString oldPropertyValue;
    rv = property->GetId(propertyID);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = property->GetNewValue(propertyValue);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = property->GetOldValue(oldPropertyValue);
    NS_ENSURE_SUCCESS(rv, rv);

    // Don't sync properties set by the device.
    if (propertyID.EqualsLiteral(SB_PROPERTY_CONTENTURL) ||
        propertyID.EqualsLiteral(SB_PROPERTY_DEVICE_PERSISTENT_ID) ||
        propertyID.EqualsLiteral(SB_PROPERTY_LAST_SYNC_PLAYCOUNT) ||
        propertyID.EqualsLiteral(SB_PROPERTY_LAST_SYNC_SKIPCOUNT)) {
      continue;
    }

    // Merge the property, ignoring errors.
    SyncMergeProperty(mediaItem, propertyID, propertyValue, oldPropertyValue);
  }

  return NS_OK;
}

/**
 * Merges the property value based on what property we're dealing with
 */
nsresult
sbBaseDevice::SyncMergeProperty(sbIMediaItem * aItem,
                                nsAString const & aPropertyId,
                                nsAString const & aNewValue,
                                nsAString const & aOldValue) {
  nsresult rv = NS_OK;

  nsString mergedValue = nsString(aNewValue);
  if (aPropertyId.Equals(NS_LITERAL_STRING(SB_PROPERTY_LASTPLAYTIME))) {
    rv = SyncMergeSetToLatest(aNewValue, aOldValue, mergedValue);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return aItem->SetProperty(aPropertyId, mergedValue);
}

/**
 * Returns the latest of the date/time. The dates are in milliseconds since
 * the JS Data's epoch date.
 */
nsresult
sbBaseDevice::SyncMergeSetToLatest(nsAString const & aNewValue,
                                   nsAString const & aOldValue,
                                   nsAString & aMergedValue) {
  nsresult rv;
  PRInt64 newDate;
  newDate = nsString_ToInt64(aNewValue, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  PRInt64 oldDate;
  oldDate = nsString_ToInt64(aOldValue, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  aMergedValue = newDate > oldDate ? aNewValue : aOldValue;
  return NS_OK;
}

nsresult
sbBaseDevice::SyncForceDiffMediaLists(sbIMediaList* aMediaList)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aMediaList);

  // Function variables.
  nsresult rv;

  // Get the list of media lists.  Do nothing if none available.
  nsCOMPtr<nsIArray> mediaListList;
  rv = aMediaList->GetItemsByProperty(NS_LITERAL_STRING(SB_PROPERTY_ISLIST),
                                      NS_LITERAL_STRING("1"),
                                      getter_AddRefs(mediaListList));
  if (NS_FAILED(rv))
    return NS_OK;

  // Set the force diff property on each media list.
  PRUint32 mediaListCount;
  rv = mediaListList->GetLength(&mediaListCount);
  NS_ENSURE_SUCCESS(rv, rv);
  for (PRUint32 i = 0; i < mediaListCount; i++) {
    // Get the media list.
    nsCOMPtr<sbIMediaList> mediaList = do_QueryElementAt(mediaListList, i, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Set the force diff property.
    rv = mediaList->SetProperty
                      (NS_LITERAL_STRING(DEVICE_PROPERTY_SYNC_FORCE_DIFF),
                       NS_LITERAL_STRING("1"));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult
sbBaseDevice::ShouldSyncMediaList(sbIMediaList* aMediaList,
                                  PRBool*       aShouldSync)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aMediaList);
  NS_ENSURE_ARG_POINTER(aShouldSync);

  // Function variables.
  nsresult rv;

  // Default to should not sync.
  *aShouldSync = PR_FALSE;

  // Don't sync download media lists.
  nsAutoString customType;
  rv = aMediaList->GetProperty(NS_LITERAL_STRING(SB_PROPERTY_CUSTOMTYPE),
                               customType);
  NS_ENSURE_SUCCESS(rv, rv);
  if (customType.EqualsLiteral("download"))
    return NS_OK;

  // Don't sync hidden lists.
  nsAutoString hidden;
  rv = aMediaList->GetProperty(NS_LITERAL_STRING(SB_PROPERTY_HIDDEN), hidden);
  NS_ENSURE_SUCCESS(rv, rv);
  if (hidden.EqualsLiteral("1"))
    return NS_OK;

  // Don't sync media lists that are storage for other media lists (e.g., simple
  // media lists for smart media lists).
  nsAutoString outerGUID;
  rv = aMediaList->GetProperty(NS_LITERAL_STRING(SB_PROPERTY_OUTERGUID),
                               outerGUID);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!outerGUID.IsEmpty())
    return NS_OK;

  // Media list should be synced.
  *aShouldSync = PR_TRUE;

  return NS_OK;
}

nsresult
sbBaseDevice::PromptForEjectDuringPlayback(PRBool* aEject)
{
  NS_ENSURE_ARG_POINTER(aEject);

  nsresult rv;

  sbPrefBranch prefBranch("songbird.device.dialog.", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  PRBool hide_dialog = prefBranch.GetBoolPref("eject_while_playing", PR_FALSE);

  if (hide_dialog) {
    // if the dialog is disabled, continue as if the user had said yes
    *aEject = PR_TRUE;
    return NS_OK;
  }

  // get the prompter service and wait for a window to be available
  nsCOMPtr<sbIPrompter> prompter =
    do_GetService("@songbirdnest.com/Songbird/Prompter;1");
  NS_ENSURE_SUCCESS(rv, rv);
  rv = prompter->SetWaitForWindow(PR_TRUE);
  NS_ENSURE_SUCCESS(rv, rv);

  // get the stringbundle
  sbStringBundle bundle;

  // get the window title
  nsString const& title = bundle.Get("device.dialog.eject_while_playing.title");

  // get the device name
  nsString deviceName;
  rv = GetName(deviceName);
  NS_ENSURE_SUCCESS(rv, rv);

  // get the message, based on the device name
  nsTArray<nsString> formatParams;
  formatParams.AppendElement(deviceName);
  nsString const& message =
    bundle.Format("device.dialog.eject_while_playing.message", formatParams);

  // get the text for the eject button
  nsString const& eject = bundle.Get("device.dialog.eject_while_playing.eject");

  // get the text for the checkbox
  nsString const& check =
    bundle.Get("device.dialog.eject_while_playing.dontask");

  // show the dialog box
  PRBool accept;
  rv = prompter->ConfirmEx(nsnull, title.get(), message.get(),
      (nsIPromptService::BUTTON_POS_0 *
       nsIPromptService::BUTTON_TITLE_IS_STRING) +
      (nsIPromptService::BUTTON_POS_1 *
       nsIPromptService::BUTTON_TITLE_CANCEL), eject.get(), nsnull, nsnull,
      check.get(), &hide_dialog, &accept);
  NS_ENSURE_SUCCESS(rv, rv);

  *aEject = !accept;

  // save the checkbox state
  rv = prefBranch.SetBoolPref("eject_while_playing", hide_dialog);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::GetPrimaryLibrary(sbIDeviceLibrary ** aDeviceLibrary)
{
  NS_ENSURE_ARG_POINTER(aDeviceLibrary);

  nsCOMPtr<sbIDeviceContent> content;
  nsresult rv = GetContent(getter_AddRefs(content));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIArray> libraries;
  rv = content->GetLibraries(getter_AddRefs(libraries));
  NS_ENSURE_SUCCESS(rv, rv);

  PRUint32 libraryCount;
  rv = libraries->GetLength(&libraryCount);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(libraryCount > 0, NS_ERROR_UNEXPECTED);

  nsCOMPtr<sbIDeviceLibrary> deviceLib =
    do_QueryElementAt(libraries, 0, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  deviceLib.forget(aDeviceLibrary);
  return NS_OK;
}

nsresult sbBaseDevice::SetDeviceWriteContentSrc
                         (sbIMediaItem* aWriteDstItem,
                          nsIURI*       aContentSrcBaseURI,
                          nsIURI*       aWriteSrcURI)
{
  // Validate arguments.
  NS_ENSURE_ARG_POINTER(aWriteDstItem);
  NS_ENSURE_ARG_POINTER(aContentSrcBaseURI);

  // Function variables.
  nsString         kIllegalChars =
                     NS_ConvertASCIItoUTF16(FILE_ILLEGAL_CHARACTERS);
  nsCOMPtr<nsIURI> writeSrcURI = aWriteSrcURI;
  nsresult         rv;

  // If no write source URI is given, get it from the write source media item.
  if (!writeSrcURI) {
    // Get the origin item for the write destination item.
    nsCOMPtr<sbIMediaItem> writeSrcItem;
    rv = sbLibraryUtils::GetOriginItem(aWriteDstItem,
                                       getter_AddRefs(writeSrcItem));
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the write source URI.
    rv = writeSrcItem->GetContentSrc(getter_AddRefs(writeSrcURI));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Get the write source file name and replace illegal characters.
  nsCOMPtr<nsIURL> writeSrcURL = do_QueryInterface(writeSrcURI, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCAutoString cWriteSrcFileName;
  rv = writeSrcURL->GetFileName(cWriteSrcFileName);
  NS_ENSURE_SUCCESS(rv, rv);
  nsAutoString writeSrcFileName = NS_ConvertUTF8toUTF16(cWriteSrcFileName);
  nsString_ReplaceChar(writeSrcFileName, kIllegalChars, PRUnichar('_'));

  // Get a file object for the content base.
  nsCOMPtr<nsIFileURL>
    contentSrcBaseFileURL = do_QueryInterface(aContentSrcBaseURI, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIFile> contentSrcBaseFile;
  rv = contentSrcBaseFileURL->GetFile(getter_AddRefs(contentSrcBaseFile));
  NS_ENSURE_SUCCESS(rv, rv);

  // Start the content source at the base.
  nsCOMPtr<nsIFile> contentSrcFile;
  rv = contentSrcBaseFile->Clone(getter_AddRefs(contentSrcFile));
  NS_ENSURE_SUCCESS(rv, rv);

  // Append file name of the write source file.
  rv = contentSrcFile->Append(writeSrcFileName);
  NS_ENSURE_SUCCESS(rv, rv);

  // Check if the content source file already exists.
  PRBool exists;
  rv = contentSrcFile->Exists(&exists);
  NS_ENSURE_SUCCESS(rv, rv);

  // Create a unique file if content source file already exists.
  if (exists) {
    // Get the permissions of the content source parent.
    PRUint32          permissions;
    nsCOMPtr<nsIFile> parent;
    rv = contentSrcFile->GetParent(getter_AddRefs(parent));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = parent->GetPermissions(&permissions);
    NS_ENSURE_SUCCESS(rv, rv);

    // Create a unique file.
    rv = contentSrcFile->CreateUnique(nsIFile::NORMAL_FILE_TYPE, permissions);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Get the device content source URI.
  nsCOMPtr<nsIURI> contentSrc;
  rv = NS_NewFileURI(getter_AddRefs(contentSrc), contentSrcFile);
  NS_ENSURE_SUCCESS(rv, rv);

  // Set the write destination item content source.
  rv = aWriteDstItem->SetContentSrc(contentSrc);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult sbBaseDevice::SetupDevice()
{
  nsresult rv;

  // Present the setup device dialog.
  nsCOMPtr<sbIPrompter>
    prompter = do_CreateInstance(SONGBIRD_PROMPTER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIDOMWindow> dialogWindow;
  rv = prompter->OpenDialog
         (nsnull,
          NS_LITERAL_STRING
            ("chrome://songbird/content/xul/device/deviceSetupDialog.xul"),
          NS_LITERAL_STRING("DeviceSetup"),
          NS_LITERAL_STRING("chrome,centerscreen,modal=yes,titlebar=no"),
          NS_ISUPPORTS_CAST(sbIDevice*, this),
          getter_AddRefs(dialogWindow));
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult 
sbBaseDevice::ProcessCapabilitiesRegistrars() 
{
  // If we haven't built the registrars then do so
  if (mCapabilitiesRegistrarType != sbIDeviceCapabilitiesRegistrar::NONE) {
    return NS_OK;
  }

  nsresult rv;
  nsCOMPtr<nsICategoryManager> catMgr =
    do_GetService(NS_CATEGORYMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISimpleEnumerator> enumerator;
  rv = catMgr->EnumerateCategory(SB_DEVICE_CAPABILITIES_REGISTRAR_CATEGORY,
                                 getter_AddRefs(enumerator));
  NS_ENSURE_SUCCESS(rv, rv);

  // Enumerate the registrars and find the highest scoring one (Greatest type)
  PRBool hasMore;
  rv = enumerator->HasMoreElements(&hasMore);
  NS_ENSURE_SUCCESS(rv, rv);

  while(hasMore) {
    nsCOMPtr<nsISupports> supports;
    rv = enumerator->GetNext(getter_AddRefs(supports));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsISupportsCString> data = do_QueryInterface(supports, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCString entryName;
    rv = data->GetData(entryName);
    NS_ENSURE_SUCCESS(rv, rv);

    char * contractId;
    rv = catMgr->GetCategoryEntry(SB_DEVICE_CAPABILITIES_REGISTRAR_CATEGORY, 
                                  entryName.get(), 
                                  &contractId);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<sbIDeviceCapabilitiesRegistrar> capabilitiesRegistrar =
      do_CreateInstance(contractId, &rv);
    NS_Free(contractId);
    NS_ENSURE_SUCCESS(rv, rv);

    PRBool interested;
    rv = capabilitiesRegistrar->InterestedInDevice(this, &interested);
    NS_ENSURE_SUCCESS(rv, rv);
    if (interested) {
      PRUint32 type;
      rv = capabilitiesRegistrar->GetType(&type);
      NS_ENSURE_SUCCESS(rv, rv);
      if (type >= mCapabilitiesRegistrarType) {
        mCapabilitiesRegistrar = capabilitiesRegistrar;
      }
    }
    
    rv = enumerator->HasMoreElements(&hasMore);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return NS_OK;
}

/**
 * The capabilities cannot be cached because the capabilities are not preserved
 * on all devices.
 */
nsresult 
sbBaseDevice::RegisterDeviceCapabilities(sbIDeviceCapabilities * aCapabilities) 
{
  NS_ENSURE_ARG_POINTER(aCapabilities);
  
  nsresult rv;
  
  rv = ProcessCapabilitiesRegistrars();
  NS_ENSURE_SUCCESS(rv, rv);
  
  if (mCapabilitiesRegistrar) {
    rv = mCapabilitiesRegistrar->AddCapabilities(this, aCapabilities);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  
  return NS_OK;
}

nsresult
sbBaseDevice::FindTranscodeProfile(sbIMediaItem * aMediaItem,
                                   sbITranscodeProfile ** aProfile) 
{
  NS_ENSURE_ARG_POINTER(aMediaItem);
  NS_ENSURE_ARG_POINTER(aProfile);
  
  nsresult rv;
  
  rv = ProcessCapabilitiesRegistrars();
  NS_ENSURE_SUCCESS(rv, rv);
  
  nsCOMPtr<sbITranscodeManager> tcManager = 
    do_GetService(SONGBIRD_TRANSCODEMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  
  nsCOMPtr<nsIArray> profiles;
  rv = tcManager->GetTranscodeProfiles(getter_AddRefs(profiles));
  NS_ENSURE_SUCCESS(rv, rv);
  
  if (mCapabilitiesRegistrar) {
    // This may return NS_ERROR_NOT_AVAILABLE or null if no transcoding is
    // required
    rv = mCapabilitiesRegistrar->ChooseProfile(aMediaItem, 
                                               profiles, 
                                               aProfile);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  else {
    // No acceptable transcoding profile available
    return NS_ERROR_NOT_AVAILABLE;
  }
  
}

/* void Format(); */
NS_IMETHODIMP sbBaseDevice::Format()
{
  return NS_ERROR_NOT_IMPLEMENTED;
}
/* readonly attribute boolean supportsReformat; */
NS_IMETHODIMP sbBaseDevice::GetSupportsReformat(PRBool *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = PR_FALSE;
  return NS_OK;
}

/* vim: set sw=2 :miv */
/*
//
// BEGIN SONGBIRD GPL
// 
// This file is part of the Songbird web player.
//
// Copyright(c) 2005-2008 POTI, Inc.
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

#include "sbDeviceEventTesterStressThreads.h"

#include <nsAutoLock.h>
#include <nsCOMPtr.h>
#include <nsServiceManagerUtils.h>
#include <nsThreadUtils.h>
#include <nsIGenericFactory.h>

#include "sbIDeviceEvent.h"
#include "sbIDeviceEventTarget.h"
#include "sbIDeviceManager.h"

NS_IMPL_THREADSAFE_ISUPPORTS2(sbDeviceEventTesterStressThreads,
                              nsIRunnable,
                              sbIDeviceEventListener)

sbDeviceEventTesterStressThreads::sbDeviceEventTesterStressThreads()
 : mCounter(-999),
   mMonitor(nsnull)
{
  /* member initializers and constructor code */
}

sbDeviceEventTesterStressThreads::~sbDeviceEventTesterStressThreads()
{
  /* destructor code */
}

/* void run (); */
NS_IMETHODIMP sbDeviceEventTesterStressThreads::Run()
{
  NS_ENSURE_FALSE(mMonitor, NS_ERROR_ALREADY_INITIALIZED);
  
  mMonitor = nsAutoMonitor::NewMonitor(__FILE__);
  NS_ENSURE_TRUE(mMonitor, NS_ERROR_OUT_OF_MEMORY);

  nsresult rv;
  
  nsCOMPtr<sbIDeviceEventTarget> target =
    do_GetService("@songbirdnest.com/Songbird/DeviceManager;2", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  
  rv = target->AddEventListener(static_cast<sbIDeviceEventListener*>(this));
  NS_ENSURE_SUCCESS(rv, rv);
  
  // spin up a *ton* of threads...
  mCounter = 0;
  for (int i = 0; i < 300; ++i) {
    nsAutoMonitor mon(mMonitor);
    nsCOMPtr<nsIRunnable> event =
      NS_NEW_RUNNABLE_METHOD(sbDeviceEventTesterStressThreads, this, OnEvent);
    NS_ENSURE_TRUE(event, NS_ERROR_OUT_OF_MEMORY);
    
    nsCOMPtr<nsIThread> thread;
    ++mCounter;
    rv = NS_NewThread(getter_AddRefs(thread), event);
    NS_ENSURE_SUCCESS(rv, rv);
    
    mThreads.AppendObject(thread);
  }
  
  // and wait for them
  while (mThreads.Count()) {
    nsCOMPtr<nsIThread> thread = mThreads[0];
    PRBool succeeded = mThreads.RemoveObjectAt(0);
    NS_ENSURE_TRUE(succeeded, NS_ERROR_FAILURE);
    rv = thread->Shutdown();
    NS_ENSURE_SUCCESS(rv, rv);
  }
  
  rv = target->RemoveEventListener(static_cast<sbIDeviceEventListener*>(this));
  NS_ENSURE_SUCCESS(rv, rv);
  
  NS_ENSURE_TRUE(mCounter == 0, NS_ERROR_FAILURE);
  
  return NS_OK;
}

/* void onDeviceEvent (in sbIDeviceEvent aEvent); */
NS_IMETHODIMP sbDeviceEventTesterStressThreads::OnDeviceEvent(sbIDeviceEvent *aEvent)
{
  nsAutoMonitor mon(mMonitor);
  --mCounter;
  if (!NS_IsMainThread()) {
    NS_WARNING("Not on main thread!");
    mCounter = -2000; // some random error value
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

void sbDeviceEventTesterStressThreads::OnEvent()
{
  nsresult rv;
  nsCOMPtr<sbIDeviceManager2> manager =
    do_GetService("@songbirdnest.com/Songbird/DeviceManager;2", &rv);
  NS_ENSURE_SUCCESS(rv, /* void */);
  
  nsCOMPtr<sbIDeviceEventTarget> target = do_QueryInterface(manager);
  NS_ENSURE_SUCCESS(rv, /* void */);
  
  nsCOMPtr<sbIDeviceEvent> event;
  rv = manager->CreateEvent(getter_AddRefs(event));
  NS_ENSURE_SUCCESS(rv, /* void */);
  
  rv = event->InitEvent(sbIDeviceEvent::EVENT_DEVICE_BASE,
                        nsnull,
                        NS_ISUPPORTS_CAST(sbIDeviceEventListener*, this));
  NS_ENSURE_SUCCESS(rv, /* void */);
  
  rv = target->DispatchEvent(event);
  NS_ENSURE_SUCCESS(rv, /* void */);
}

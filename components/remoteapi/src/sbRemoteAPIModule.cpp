/*
//
// BEGIN SONGBIRD GPL
//
// This file is part of the Songbird web player.
//
// Copyright(c) 2005-2007 POTI, Inc.
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

#include "sbRemoteCommands.h"
#include "sbRemoteLibrary.h"
#include "sbRemotePlayer.h"
#include "sbSecurityMixin.h"

#define SONGBIRD_REMOTEAPI_MODULENAME "Songbird Remote API Module"

NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(sbRemoteLibrary, Init)
NS_DECL_CI_INTERFACE_GETTER(sbRemoteLibrary)

NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(sbRemoteCommands, Init)
NS_DECL_CI_INTERFACE_GETTER(sbRemoteCommands)

NS_GENERIC_FACTORY_CONSTRUCTOR(sbSecurityMixin)
NS_DECL_CI_INTERFACE_GETTER(sbSecurityMixin)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(sbRemotePlayer, sbRemotePlayer::GetInstance)
NS_DECL_CI_INTERFACE_GETTER(sbRemotePlayer)

// fill out data struct to register with component system
static const nsModuleComponentInfo components[] =
{
  {
    SONGBIRD_REMOTECOMMANDS_CLASSNAME,
    SONGBIRD_REMOTECOMMANDS_CID,
    SONGBIRD_REMOTECOMMANDS_CONTRACTID,
    sbRemoteCommandsConstructor,
    NULL,
    NULL,
    NULL,
    NS_CI_INTERFACE_GETTER_NAME(sbRemoteCommands)
  },
  {
    SONGBIRD_REMOTELIBRARY_CLASSNAME,
    SONGBIRD_REMOTELIBRARY_CID,
    SONGBIRD_REMOTELIBRARY_CONTRACTID,
    sbRemoteLibraryConstructor,
    NULL,
    NULL,
    NULL,
    NS_CI_INTERFACE_GETTER_NAME(sbRemoteLibrary)
  },
  {
    SONGBIRD_REMOTEPLAYER_CLASSNAME,
    SONGBIRD_REMOTEPLAYER_CID,
    SONGBIRD_REMOTEPLAYER_CONTRACTID,
    sbRemotePlayerConstructor,
    sbRemotePlayer::Register,
    sbRemotePlayer::Unregister,
    NULL,
    NS_CI_INTERFACE_GETTER_NAME(sbRemotePlayer)
  },
  {
    SONGBIRD_SECURITYMIXIN_CLASSNAME,
    SONGBIRD_SECURITYMIXIN_CID,
    SONGBIRD_SECURITYMIXIN_CONTRACTID,
    sbSecurityMixinConstructor,
    NULL,
    NULL,
    NULL,
    NS_CI_INTERFACE_GETTER_NAME(sbSecurityMixin)
  }
};

// create the module info struct that is used to regsiter
NS_IMPL_NSGETMODULE(SONGBIRD_REMOTEAPI_MODULENAME, components)


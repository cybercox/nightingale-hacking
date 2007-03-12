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

/**
 * \brief Test file
 */

function countItems(enumerator) {
  var count = 0;
  while (enumerator.hasMoreElements()) {
    var item = enumerator.getNext().QueryInterface(Ci.sbIMediaItem);
    assertNotEqual(item, null);
    count++;
  }
  return count;
}

function runTest () {

  var databaseGUID = "test_localdatabaselibrary";
  createDatabase(databaseGUID);

  var library = createLibrary(databaseGUID);

  var list = library.getMediaItem("7e8dcc95-7a1d-4bb3-9b14-d4906a9952cb");
  assertList(list, "data_sort_sml101_ordinal_asc.txt");
  
  var titleProperty = "http://songbirdnest.com/data/1.0#trackName";
  var albumProperty = "http://songbirdnest.com/data/1.0#albumName";
  var genreProperty = "http://songbirdnest.com/data/1.0#genre";
  
  var filteredListEnumerator =
    list.getItemsByPropertyValue(titleProperty, "Train of Thought");
  
  assertEqual(countItems(filteredListEnumerator), 1);
  
  filteredListEnumerator =
    list.getItemsByPropertyValue(albumProperty, "Back in Black");

  assertEqual(countItems(filteredListEnumerator), 10);
  
  filteredListEnumerator =
    list.getItemsByPropertyValue(genreProperty, "KJaskjjbfjJDBs");
    
  assertEqual(countItems(filteredListEnumerator), 0);

  var propertyArray =
    Cc["@songbirdnest.com/Songbird/Properties/PropertyArray;1"].
    createInstance(Ci.sbIPropertyArray);
  
  propertyArray.appendProperty(albumProperty, "Back in Black");
  filteredListEnumerator =
    list.getItemsByPropertyValues(propertyArray);
    
  assertEqual(countItems(filteredListEnumerator), 10);
  
  propertyArray.appendProperty(titleProperty, "Rock and Roll Ain't Noise Pollution");
  propertyArray.appendProperty(titleProperty, "Shake a Leg");
  filteredListEnumerator =
    list.getItemsByPropertyValues(propertyArray);

  assertEqual(countItems(filteredListEnumerator), 2);
  
  propertyArray.removeElementAt(1);
  filteredListEnumerator =
    list.getItemsByPropertyValues(propertyArray);
  
  assertEqual(countItems(filteredListEnumerator), 1);
}

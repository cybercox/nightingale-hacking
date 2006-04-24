/*
//
// BEGIN SONGBIRD GPL
// 
// This file is part of the Songbird web player.
//
// Copyright� 2006 Pioneers of the Inevitable LLC
// http://songbirdnest.com
// 
// This file may be licensed under the terms of of the
// GNU General Public License Version 2 (the �GPL�).
// 
// Software distributed under the License is distributed 
// on an �AS IS� basis, WITHOUT WARRANTY OF ANY KIND, either 
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

//
// sbIDataRemote wrapper
//
//  This object provides the ability to set key-value pairs
//  into a global "data store," and to have various callback
//  effects occur when anyone changes an observed value.
//
//  The callback binding can be placed on a dom element to
//  change its properties or attributes based upon the value
//  (either directly, or as a boolean, and/or as the result
//  of a given evaluation expression).
//
//  The mozilla preferences system is used as the underlying
//  data storage layer to power this interface.  Because the
//  preferences are available to all open windows in xulrunner,
//  these data remotes should will function as globals across 
//  the application.  This is both powerful and dangerous, and
//  while this interface is available to all, everyone should
//  be very careful to properly namespace their data strings.
//
//  SBDataBindElementProperty Param List:
//   key  - The data ID to bind upon
//   elem - The element ID to be bound
//   attr - The name of the property or attribute to change
//   bool - Optionally assign the data as BOOLEAN data (true/false props, "true"/"false" attrs)
//   not  - Optionally assign the data as a boolean NOT of the value
//   eval - Optionally apply an eval string where `value = eval( eval_string );`

function sbIDataRemote( key, root )
{
  try
  {
    //
    // Data
    //

    // Set the strings
    if ( root = null )
    {
      this.m_Root = "songbird.dataremotes." + key; // Use key in root string, makes unique observer lists per key (big but fast?).
      this.m_Key = key;
    }
    else
    {
      // If we're specifying a root, just obey what's asked for.
      this.m_Root = root;
      this.m_Key = key;
    }
    
    // Set the callbacks
    this.m_CallbackFunction = null;
    this.m_CallbackObject = null;
    this.m_CallbackPropery = null;
    this.m_CallbackAttribute = null;
    this.m_CallbackBool = false;
    this.m_CallbackNot = false;
    this.m_CallbackEval = "";
    this.m_CallbackObserving = false;
    
    // Go connect to the prefs object acting as the data representation of the key.
    prefs_service = SBBindInterface( null, "@mozilla.org/preferences;1", Components.interfaces.nsIPrefService, true );
    if ( prefs_service )
    {
      // Ask for the branch.
      this.m_Prefs = prefs_service.getBranch( this.m_Root );
      if ( this.m_Prefs )
      {
        this.m_Prefs = this.m_Prefs.QueryInterface( Components.interfaces.nsIPrefBranch2 );
      }
    }
    
    //
    // Methods
    //

    // Query Interface - So we can act as an observer object.
    this.QueryInterface = function( aIID )
    {
      if (!aIID.equals(Components.interfaces.nsIObserver) &&
          !aIID.equals(Components.interfaces.nsISupportsWeakReference) &&
          !aIID.equals(Components.interfaces.nsISupports)) 
      {
        throw Components.results.NS_ERROR_NO_INTERFACE;
      }
      
      return this;
    };
    
    // observe - Called when someone updates the remote data
    this.observe = function( subject, topic, data )
    { 
      try
      {
        if ( this.m_Prefs )
        {
          if ( data == this.m_Key )
          {
            if ( this.m_SuppressFirst )
            {
              this.m_SuppressFirst = false;
              return;
            }
            // Get the value (why isn't this a param?)
            var value = this.GetValue();
            
            // Run the optional evaluation
            if ( this.m_CallbackEval.length )
            {
              value = eval( this.m_CallbackEval );
            }
            
            // Handle boolean and not states
            if ( this.m_CallbackBool )
            {
              // If we're not bool before,
              if( typeof( value ) != "boolean" )
              {
                if ( value == "true" )
                {
                  value = true;
                }
                else if ( value == "false" )
                {
                  value = false;
                }
                else
                {
                  value = ( this.MakeIntValue( value ) != 0 );
                }
              }
              // ...we are now!
              if ( this.m_CallbackNot )
              {
                value = ! value;
              }
            }
            
/*            
            if ( this.m_Key == "playlist.repeat" )
            {
              var obj = "No object";
              if ( this.m_CallbackObject )
              {
                obj = this.m_CallbackObject.id;
              }
              alert( obj + " - " + this.m_CallbackPropery + " - " + this.m_CallbackAttribtue + " - " + value );
            }  
*/
            
            // Handle callback states
            if ( this.m_CallbackFunction )
            {
              // Call the callback function
              this.m_CallbackFunction( value );
            }
            else if ( this.m_CallbackObject && this.m_CallbackPropery )
            {
              // Set the callback object's property
              this.m_CallbackObject[ this.m_CallbackPropery ] = value;
            }
            else if ( this.m_CallbackObject && this.m_CallbackAttribute )
            {
              var val_str = value;
              // If bool-type, convert to string.
              if ( this.m_CallbackBool )
              {
                if ( value )
                {
                  val_str = "true";
                }
                else
                {
                  val_str = "false";
                }
              }
              // Set the callback object's attribute
              this.m_CallbackObject.setAttribute( this.m_CallbackAttribute, val_str );
            }
          }
        }
      }
      catch( err ) 
      {
        alert( err );
      }
    };
    
    this.Unbind = function()
    {
      try
      {
        if ( this.m_Prefs )
        {
          // Clear ourselves as an observer.
          if ( this.m_CallbackObserving )
            this.m_Prefs.removeObserver( this.m_Key, this );
          this.m_CallbackObserving = false;
        }
      }
      catch ( err )
      {
        alert( err );
      }
    }
    
    this.BindEventFunction = function( func )
    {
      this.BindCallbackFunction( func, true );
    }
    
    this.BindCallbackFunction = function( func, suppress_first )
    {
      try
      {
        if ( this.m_Prefs )
        {
          // Don't call the function the first time it is bound
          this.m_SuppressFirst = suppress_first;
        
          // Clear and reinsert ourselves as an observer.
          if ( this.m_CallbackObserving )
            this.m_Prefs.removeObserver( this.m_Key, this );
          this.m_Prefs.addObserver( this.m_Key, this, true );
          this.m_CallbackObserving = true;

          // Now we're observing for a function.        
          this.m_CallbackFunction = func;
          this.m_CallbackObject = null;
          this.m_CallbackPropery = null;
          this.m_CallbackAttribute = null;
          this.m_CallbackBool = false;
          this.m_CallbackNot = false;
          this.m_CallbackEval = "";
          
          // Set the value once
          this.observe( null, null, this.m_Key );
            
/*            
          if ( this.m_Key == "playlist.repeat" )
          {
            var obj = "No object";
            if ( this.m_CallbackObject )
            {
              obj = this.m_CallbackObject.id;
            }
            alert( obj + " - " + this.m_CallbackPropery + " - " + this.m_CallbackAttribtue + " - " + value );
          }  
*/          
        }
      }
      catch ( err )
      {
        alert( err );
      }
    };
    
    this.BindCallbackProperty = function( obj, prop, bool, not, eval )
    {
      try
      {
        if ( ! bool )
        {
          bool = false;
        }
        if ( ! not )
        {
          not = false;
        }
        if ( ! eval )
        {
          eval = "";
        }
        if ( this.m_Prefs )
        {
          // Clear and reinsert ourselves as an observer.
          if ( this.m_CallbackObserving )
            this.m_Prefs.removeObserver( this.m_Key, this );
          this.m_Prefs.addObserver( this.m_Key, this, true );
          this.m_CallbackObserving = true;
          
          // Now we're observing for an object's property.        
          this.m_CallbackFunction = null;
          this.m_CallbackObject = obj;
          this.m_CallbackPropery = prop;
          this.m_CallbackAttribute = null;
          this.m_CallbackBool = bool;
          this.m_CallbackNot = not;
          this.m_CallbackEval = eval;
          
          // Set the value once
          this.observe( null, null, this.m_Key );

/*            
          if ( this.m_Key == "playlist.repeat" )
          {
            var obj = "No object";
            if ( this.m_CallbackObject )
            {
              obj = this.m_CallbackObject.id;
            }
            alert( obj + " - " + this.m_CallbackPropery + " - " + this.m_CallbackAttribtue + " - " + value );
          }  
*/          
        }
      }
      catch ( err )
      {
        alert( err );
      }
    };
    
    this.BindCallbackAttribute = function( obj, attr, bool, not, eval )
    {
      try
      {
        if ( ! bool )
        {
          bool = false;
        }
        if ( ! not )
        {
          not = false;
        }
        if ( ! eval )
        {
          eval = "";
        }
        if ( this.m_Prefs )
        {
          // Clear and reinsert ourselves as an observer.
          if ( this.m_CallbackObserving )
            this.m_Prefs.removeObserver( this.m_Key, this );
          this.m_Prefs.addObserver( this.m_Key, this, true );
          this.m_CallbackObserving = true;
          
          // Now we're observing for an object's attribute.        
          this.m_CallbackFunction = null;
          this.m_CallbackObject = obj;
          this.m_CallbackPropery = null;
          this.m_CallbackAttribute = attr;
          this.m_CallbackBool = bool;
          this.m_CallbackNot = not;
          this.m_CallbackEval = eval;
          
          // Set the value once
          this.observe( null, null, this.m_Key );
        }
      }
      catch ( err )
      {
        alert( err );
      }
    };

    // SetValue - Put the value into the data store, alert everyone watching this data    
    this.SetValue = function( value )
    {
      // Clear the value
      if ( value == null )
      {
        value = "";
      }
      // Convert from boolean to intstring.
      if( typeof( value ) == "boolean" )
      {
        if ( value )
        {
          value = "1";
        }
        else
        {
          value = "0";
        }
      }
      try
      {
        if ( this.m_Prefs )
        {
          // Make a unicode string, assign the value, set it into the preferences.
          var sString = Components.classes["@mozilla.org/supports-string;1"].createInstance(Components.interfaces.nsISupportsString);
          sString.data = value;
          this.m_Prefs.setComplexValue( this.m_Key, Components.interfaces.nsISupportsString, sString );
        }
      }
      catch ( err )
      {
        alert( err );
      }
    };

    // GetValue - Get the value from the data store
    this.GetValue = function()
    {
      var retval = "";
      try
      {
        if ( this.m_Prefs )
        {
          if ( this.m_Prefs.prefHasUserValue( this.m_Key ) )
          {
            retval = this.m_Prefs.getComplexValue( this.m_Key, Components.interfaces.nsISupportsString ).data;
          }
        }
      }
      catch ( err )
      {
        alert( err );
      }
      return retval;
    };
    
    // GetIntValue - Get the value from the data store as an int
    this.GetIntValue = function()
    {
      return this.MakeIntValue( this.GetValue() );
    }
    
    // MakeIntValue - Get the value from the data store as an int
    this.MakeIntValue = function( value )
    {
      var retval = 0;
      try
      {
        if ( value && value.length )
        {
          retval = parseInt( value );
        }
      }
      catch ( err )
      {
        alert( err );
      }
      return retval;
    };

  }
  catch ( err )
  {
    alert( err );
  }

  return this;
}

function SBDataBindElementProperty( key, elem, prop, bool, not, eval )
{
  var retval = null;
  try
  {
    var obj = document.getElementById( elem );
    if ( obj )
    {
      retval = new sbIDataRemote( key );
      retval.BindCallbackProperty( obj, prop, bool, not, eval );
    }
    else
    {
      alert( "Can't find " + elem );
    }
  }
  catch ( err )
  {
    alert( err );
  }
  return retval;  
}
function SBDataBindElementAttribute( key, elem, attr, bool, not, eval )
{
  var retval = null;
  try
  {
    var obj = document.getElementById( elem );
    if ( obj )
    {
      retval = new sbIDataRemote( key );
      retval.BindCallbackAttribute( obj, attr, bool, not, eval );
    }
    else
    {
      alert( "Can't find " + elem );
    }
  }
  catch ( err )
  {
    alert( err );
  }
  return retval;  
}

// Some XBL contexts don't create an object properly outside this script.
function SBDataGetValue( key )
{
  var data = new sbIDataRemote( key );
  return data.GetValue();
}

function SBDataGetIntValue( key )
{
  var data = new sbIDataRemote( key );
  return data.GetIntValue();
}

function SBDataSetValue( key, value )
{
  var data = new sbIDataRemote( key );
  return data.SetValue( value );
}

function SBDataFireEvent( key )
{
  var data = new sbIDataRemote( key );
  return data.SetValue( data.GetIntValue() + 1 );
}

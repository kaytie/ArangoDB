////////////////////////////////////////////////////////////////////////////////
/// @brief Utf8/Utf16 Helper class
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2012 triagens GmbH, Cologne, Germany
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
/// @author Frank Celler
/// @author Achim Brandt
/// @author Copyright 2010-2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_BASICS_UTF8_HELPER_H
#define TRIAGENS_BASICS_UTF8_HELPER_H 1

#include "Basics/Common.h"

#ifdef TRI_HAVE_ICU
#include "unicode/coll.h"
#endif
// -----------------------------------------------------------------------------
// --SECTION--                                                 class WriteLocker
// -----------------------------------------------------------------------------

namespace triagens {
  namespace basics {

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Utf8Helper
/// @{
////////////////////////////////////////////////////////////////////////////////

    class Utf8Helper {
        Utf8Helper (Utf8Helper const&);
        Utf8Helper& operator= (Utf8Helper const&);
    
      public:
        
////////////////////////////////////////////////////////////////////////////////
///  static functions
////////////////////////////////////////////////////////////////////////////////

#ifdef TRI_HAVE_ICU
        
////////////////////////////////////////////////////////////////////////////////
/// @brief normalize an utf8 string (NFC)
////////////////////////////////////////////////////////////////////////////////

        static char * normalizeUtf8 (TRI_memory_zone_t* zone, const char* utf8, size_t inLength, size_t* outLength);

////////////////////////////////////////////////////////////////////////////////
/// @brief normalize an utf16 string (NFC) and export it to utf8
////////////////////////////////////////////////////////////////////////////////

        static char * normalizeUtf16 (TRI_memory_zone_t* zone, const uint16_t* utf16, size_t inLength, size_t* outLength);

#endif
        
      public:
        
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor 
/// @param string lang                     Use "de_DE", "en_US" or "" (default)
////////////////////////////////////////////////////////////////////////////////
        
        Utf8Helper(const string& lang);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////
        
        ~Utf8Helper();

      public:
        
////////////////////////////////////////////////////////////////////////////////
///  public functions
////////////////////////////////////////////////////////////////////////////////
        
////////////////////////////////////////////////////////////////////////////////
/// @brief compare utf8 strings
/// -1 : left < right
///  0 : left == right
///  1 : left > right
////////////////////////////////////////////////////////////////////////////////

        int compareUtf8 (const char* left, size_t leftLength, const char* right, size_t rightLength);

////////////////////////////////////////////////////////////////////////////////
/// @brief compare utf16 strings
/// -1 : left < right
///  0 : left == right
///  1 : left > right
////////////////////////////////////////////////////////////////////////////////

        int compareUtf16 (const uint16_t* left, size_t leftLength, const uint16_t* right, size_t rightLength);

    private:
#ifdef TRI_HAVE_ICU      
        Collator* _coll;
#else
        char* _coll;
#endif        
    };
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

  }
}


#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

/*
 * Utils.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <core/libclang/Utils.hpp>

#include <core/libclang/LibClang.hpp>

namespace core {
namespace libclang {

// note that this function disposes the underlying CXString so it
// shouldn't be used after this call
std::string toStdString(CXString cxStr)
{
   std::string str(clang().getCString(cxStr));
   clang().disposeString(cxStr);
   return str;
}

} // namespace libclang
} // namespace core


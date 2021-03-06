/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include "compile/Method.hpp"
#include "il/DataTypes.hpp"
#include "gtest/gtest.h"
#include "OMRTestEnv.hpp"

#ifdef MS_WINDOWS
#undef BYTE
#include "windows.h"
#define PATH_MAX MAXPATHLEN
#else
#include <dlfcn.h>
#endif

int main(int argc, char **argv)
   {
   ::testing::InitGoogleTest(&argc, argv);
   ::testing::AddGlobalTestEnvironment(new TestCompiler::OMRTestEnv);
   return RUN_ALL_TESTS();
   }

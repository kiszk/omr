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

#include "x/codegen/X86FPConversionSnippet.hpp"

#include <stddef.h>                              // for NULL
#include <stdint.h>                              // for uint8_t, int32_t, etc
#include "codegen/CodeGenerator.hpp"             // for CodeGenerator, etc
#include "codegen/FrontEnd.hpp"                  // for TR_FrontEnd
#include "codegen/Linkage.hpp"
#include "codegen/Machine.hpp"                   // for Machine
#include "codegen/RealRegister.hpp"              // for RealRegister, etc
#include "codegen/RegisterConstants.hpp"
#include "codegen/Snippet.hpp"                   // for commentString, etc
#include "codegen/SnippetGCMap.hpp"
#include "compile/Compilation.hpp"               // for Compilation
#include "env/IO.hpp"
#include "env/jittypes.h"                        // for intptrj_t
#include "il/DataTypes.hpp"                      // for FLOAT_NAN
#include "il/ILOpCodes.hpp"                      // for ILOpCodes::f2i, etc
#include "il/Node.hpp"                           // for Node
#include "il/SymbolReference.hpp"                // for SymbolReference
#include "il/symbol/LabelSymbol.hpp"             // for LabelSymbol
#include "infra/Assert.hpp"                      // for TR_ASSERT
#include "ras/Debug.hpp"                         // for TR_Debug
#include "runtime/Runtime.hpp"                   // for ::TR_HelperAddress
#include "x/codegen/X86Instruction.hpp"

uint8_t *TR_X86FPConversionSnippet::emitSnippetBody()
   {
   // *this    swipeable for debugging purposes
   uint8_t *buffer = cg()->getBinaryBufferCursor();
   getSnippetLabel()->setCodeLocation(buffer);
   return genRestartJump(genFPConversion(buffer));
   }


uint8_t *TR_X86FPConversionSnippet::emitCallToConversionHelper(uint8_t *buffer)
   {
   // *this    swipeable for debugging purposes

   *buffer++ = 0xe8;      // CallImm4

   intptrj_t helperAddress = (intptrj_t)getHelperSymRef()->getMethodAddress();
   if (NEEDS_TRAMPOLINE(helperAddress, buffer+4, cg()))
      {
      helperAddress = cg()->fe()->indexedTrampolineLookup(getHelperSymRef()->getReferenceNumber(), (void *)buffer);
      TR_ASSERT(IS_32BIT_RIP(helperAddress, buffer+4), "Local helper trampoline should be reachable directly.\n");
      }
   *(int32_t *)buffer = (int32_t)(helperAddress - (intptrj_t)(buffer+4));
   cg()->addProjectSpecializedRelocation(buffer, (uint8_t *)getHelperSymRef(), NULL, TR_HelperAddress,
                                                         __FILE__, __LINE__, getNode());
   buffer += 4;
   gcMap().registerStackMap(buffer, cg());
   return buffer;
   }


uint8_t *TR_X86FPConvertToIntSnippet::genFPConversion(uint8_t *buffer)
   {
   // *this    swipeable for debugging purposes

   TR::ILOpCodes              opcode          = _convertInstruction->getNode()->getOpCodeValue();
   TR::RealRegister          *targetRegister  = toRealRegister(_convertInstruction->getTargetRegister());
   TR::RealRegister::RegNum   targetReg       = targetRegister->getRegisterNumber();

   TR::Machine *machine = cg()->machine();

   TR_ASSERT(cg()->getProperties().getIntegerReturnRegister() == TR::RealRegister::eax, "Only support integer return in eax");

   if (targetReg != TR::RealRegister::eax)
      {
      // MOV R, eax
      //
      *buffer++ = 0x8b;
      *buffer = 0xc0;
      targetRegister->setRegisterFieldInModRM(buffer);
      buffer++;
      }

   // Push the floating-point value on to the stack before calling the helper.
   //
   // SUB esp, 4/8
   //
   *buffer++ = 0x83;
   *buffer++ = 0xec;
   if (opcode == TR::f2i)
      *buffer++ = 0x04;
   else
      *buffer++ = 0x08;

   if (_convertInstruction->getIA32RegMemInstruction())
      {
      // FST [esp], st0
      //
      if (opcode == TR::f2i)
         *buffer++ = 0xd9;
      else
         *buffer++ = 0xdd;
      *buffer++ = 0x14;
      *buffer++ = 0x24;
      }
   else
      {
      TR::X86RegRegInstruction  *instr = _convertInstruction->getIA32RegRegInstruction();
      TR_ASSERT(instr != NULL, "f2i conversion instruction must be either L4RegMem or CVTTSS2SIRegReg\n");

      TR::RealRegister *sourceRegister = toRealRegister(instr->getSourceRegister());

      // MOVSS/MOVSD [esp], source
      //
      if (opcode == TR::f2i)
         *buffer++ = 0xf3;
      else
         *buffer++ = 0xf2;
      *buffer++ = 0x0f;
      *buffer++ = 0x11;
      *buffer = 0x04;
      sourceRegister->setRegisterFieldInModRM(buffer);
      buffer++;
      *buffer++ = 0x24;
      }

   // Call the helper
   //
   buffer = emitCallToConversionHelper(buffer);

   // ADD esp, 4/8
   //
   *buffer++ = 0x83;
   *buffer++ = 0xc4;
   if (opcode == TR::f2i)
      *buffer++ = 0x04;
   else
      *buffer++ = 0x08;

   if (targetReg != TR::RealRegister::eax)
      {
      // XCHG R, eax
      //
      *buffer = 0x90;
      targetRegister->setRegisterFieldInOpcode(buffer);
      buffer++;
      }

   return buffer;
   }


void
TR_Debug::print(TR::FILE *pOutFile, TR_X86FPConvertToIntSnippet  * snippet)
   {
   // *this    swipeable for debugging purposes

   if (pOutFile == NULL)
      return;

   uint8_t *bufferPos = snippet->getSnippetLabel()->getCodeLocation();

   printSnippetLabel(pOutFile, snippet->getSnippetLabel(), bufferPos, getName(snippet));

   TR::RealRegister *targetRegister = toRealRegister(snippet->getConvertInstruction()->getTargetRegister());
   uint8_t              reg = targetRegister->getRegisterNumber();

   if (reg != TR::RealRegister::eax)
      {
      printPrefix(pOutFile, NULL, bufferPos, 2);
      trfprintf(pOutFile, "mov\t");
      print(pOutFile, targetRegister, TR_WordReg);
      trfprintf(pOutFile, ", eax\t\t%s preserve helper return reg",
                    commentString());
      bufferPos += 2;
      }

   printPrefix(pOutFile, NULL, bufferPos, 3);
   trfprintf(pOutFile, "sub\tesp, 4\t\t%s push parameter",
                 commentString());
   bufferPos += 3;

   TR::X86RegRegInstruction  *instr = snippet->getConvertInstruction()->getIA32RegRegInstruction();

   if (instr)
      {
      printPrefix(pOutFile, NULL, bufferPos, 5);
      trfprintf(pOutFile, "movss\t dword ptr [esp], ");
      print(pOutFile, toRealRegister(instr->getSourceRegister()), TR_QuadWordReg);
      bufferPos += 5;
      }
   else
      {
      printPrefix(pOutFile, NULL, bufferPos, 3);
      trfprintf(pOutFile, "fst\tdword ptr [esp]");
      bufferPos += 3;
      }

   printPrefix(pOutFile, NULL, bufferPos, 5);
   trfprintf(pOutFile, "call\t%s", getName(snippet->getHelperSymRef()));
   bufferPos += 5;

   printPrefix(pOutFile, NULL, bufferPos, 3);
   trfprintf(pOutFile, "add\tesp, 4\t\t%s pop parameter",
                 commentString());
   bufferPos += 3;

   if (reg != TR::RealRegister::eax)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "xchg\teax, ");
      print(pOutFile, targetRegister, TR_WordReg);
      trfprintf(pOutFile, "\t\t%s restore eax",
                    commentString());
      bufferPos++;
      }

   printRestartJump(pOutFile, snippet, bufferPos);
   }


uint32_t TR_X86FPConvertToIntSnippet::getLength(int32_t estimatedSnippetStart)
   {
   // *this    swipeable for debugging purposes
   uint32_t length = 11;
   TR::Machine * machine = cg()->machine();


   if (toRealRegister(_convertInstruction->getTargetRegister())->getRegisterNumber() != TR::RealRegister::eax)
      {
      // MOV   R, eax
      // XCHG  R, eax
      //
      length += 3;
      }

   if (_convertInstruction->getIA32RegMemInstruction())
      {
      // FST [esp], st0
      length += 3;
      }
   else
      {
      // MOVSS [esp], R
      length += 5;
      }

   return length + estimateRestartJumpLength(estimatedSnippetStart + length);
   }


// Each _registerAction is a union of actionFlags
//
const uint8_t TR_X86FPConvertToLongSnippet::_registerActions[16] =
   { 0x1e,   // 0
     0x14,   // 1
     0x0e,   // 2
     0x00,   // 3
     0x13,   // 4
     0x00,   // 5
     0x01,   // 6
     0x00,   // 7
     0x0a,   // 8
     0x00,   // 9
     0x00,   // 10
     0x00,   // 11
     0x00,   // 12
     0x00,   // 13
     0x00,   // 14
     0x00};  // 15


uint8_t *TR_X86FPConvertToLongSnippet::genFPConversion(uint8_t *buffer)
   {
   // *this    swipeable for debugging purposes

   // Mask off the FXCH flag.
   //
   uint8_t action = _registerActions[ _action & 0x7f ];

   if (_action & kNeedFXCH)
      {
      *buffer++ = 0xd9;                         // FXCH st(i)
      *buffer = 0xc8;
      _doubleRegister->setRegisterFieldInOpcode(buffer);
      buffer++;
      }

   if (action & kPreserveEAX)
      {
      *buffer++ = 0x50;                         // PUSH eax
      }

   if (action & kPreserveEDX)
      {
      *buffer++ = 0x52;                         // PUSH edx
      }

   buffer = emitCallToConversionHelper(buffer);

   if (action & kMOVLow)
      {
      *buffer++ = 0x8b;                         // MOV RL, eax
      *buffer = 0xc0;
      _lowRegister->setRegisterFieldInModRM(buffer);
      buffer++;
      }

   if (action & kMOVHigh)
      {
      *buffer++ = 0x8b;                         // MOV RH, edx
      *buffer = 0xc2;
      _highRegister->setRegisterFieldInModRM(buffer);
      buffer++;
      }

   if (action & kXCHG)
      {
      *buffer++ = 0x92;                         // XCHG eax, edx
      }

   if (action & kPreserveEDX)
      {
      *buffer++ = 0x5a;                         // POP edx
      }

   if (action & kPreserveEAX)
      {
      *buffer++ = 0x58;                         // POP eax
      }

   if (_action & kNeedFXCH)
      {
      *buffer++ = 0xd9;                         // FXCH st(i)
      *buffer = 0xc8;
      _doubleRegister->setRegisterFieldInOpcode(buffer);
      buffer++;
      }

   return buffer;
   }


void
TR_Debug::print(TR::FILE *pOutFile, TR_X86FPConvertToLongSnippet  * snippet)
   {
   // *this    swipeable for debugging purposes

   if (pOutFile == NULL)
      return;

   uint8_t *bufferPos = snippet->getSnippetLabel()->getCodeLocation();

   uint8_t action = TR_X86FPConvertToLongSnippet::_registerActions[snippet->getAction() & 0x7f ];

   printSnippetLabel(pOutFile, snippet->getSnippetLabel(), bufferPos, getName(snippet));

   if (snippet->getAction() & snippet->kNeedFXCH)
      {
      printPrefix(pOutFile, NULL, bufferPos, 2);
      trfprintf(pOutFile, "fxch\t");
      print(pOutFile, snippet->getDoubleRegister(), TR_FloatReg);
      trfprintf(pOutFile, "\t\t%s register to convert",
                 commentString());
      bufferPos += 2;
      }

   if (action & snippet->kPreserveEAX)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "push\teax\t\t%s preserve eax",
                    commentString());
      bufferPos++;
      }

   if (action & snippet->kPreserveEDX)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "push\tedx\t\t%s preserve eax",
                    commentString());
      bufferPos++;
      }

   printPrefix(pOutFile, NULL, bufferPos, 5);
   trfprintf(pOutFile, "call\t%s", getName(snippet->getHelperSymRef()));
   bufferPos += 5;

   if (action & snippet->kMOVLow)
      {
      printPrefix(pOutFile, NULL, bufferPos, 2);
      trfprintf(pOutFile, "mov\t");
      print(pOutFile, snippet->getLowRegister(), TR_WordReg);
      trfprintf(pOutFile, ", eax\t%s result register (low)",
                    commentString());
      bufferPos += 2;
      }

   if (action & snippet->kMOVHigh)
      {
      printPrefix(pOutFile, NULL, bufferPos, 2);
      trfprintf(pOutFile, "mov\t");
      print(pOutFile, snippet->getHighRegister(), TR_WordReg);
      trfprintf(pOutFile, ", edx\t%s result register (high)",
                    commentString());
      bufferPos += 2;
      }

   if (action & snippet->kXCHG)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "xchg\teax, edx");
      bufferPos += 1;
      }

   if (action & snippet->kPreserveEDX)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "pop\tedx\t\t%s restore edx",
                    commentString());
      bufferPos += 1;
      }

   if (action & snippet->kPreserveEAX)
      {
      printPrefix(pOutFile, NULL, bufferPos, 1);
      trfprintf(pOutFile, "pop\teax\t\t%s restore eax",
                    commentString());
      bufferPos += 1;
      }

   if (snippet->getAction() & snippet->kNeedFXCH)
      {
      printPrefix(pOutFile, NULL, bufferPos, 2);
      trfprintf(pOutFile, "fxch\t");
      print(pOutFile, snippet->getDoubleRegister(), TR_FloatReg);
      bufferPos += 2;
      }

   printRestartJump(pOutFile, snippet, bufferPos);
   }



void TR_X86FPConvertToLongSnippet::analyseLongConversion()
   {
   // *this    swipeable for debugging purposes

   // The current assumption is that register assignment will occur prior
   // to the snippets being sized and emitted.
   //
   TR_ASSERT(((_loadHighInstruction && _loadHighInstruction->getTargetRegister()) &&
            (_loadLowInstruction && _loadLowInstruction->getTargetRegister()) &&
            (_clobberInstruction && _clobberInstruction->getSourceRegister())),
           "analyseLongConversion() ==> register assignment is a prerequisite!\n");

   _action = 0;

   _lowRegister = toRealRegister(_loadLowInstruction->getTargetRegister());
   _highRegister = toRealRegister(_loadHighInstruction->getTargetRegister());
   _doubleRegister = toRealRegister(_clobberInstruction->getSourceRegister());

   TR::Machine * machine = cg()->machine();

   _action |= ((_doubleRegister->getRegisterNumber() != TR::RealRegister::st0) << 7);
   _action |= ((_lowRegister->getRegisterNumber() == TR::RealRegister::eax) << 3);
   _action |= ((_lowRegister->getRegisterNumber() == TR::RealRegister::edx) << 2);
   _action |= ((_highRegister->getRegisterNumber() == TR::RealRegister::eax) << 1);
   _action |= (_highRegister->getRegisterNumber() == TR::RealRegister::edx);
   }


uint32_t TR_X86FPConvertToLongSnippet::getLength(int32_t estimatedSnippetStart)
   {
   // *this    swipeable for debugging purposes
   uint32_t length = 5;

   analyseLongConversion();

   // Mask off the FXCH flag.
   //
   uint8_t action = _registerActions[ _action & 0x7f ];

   if (_action & kNeedFXCH)
      {
      // symmetric FXCHs
      //
      length += 4;
      }

   if (action & kPreserveEAX)
      {
      // PUSH eax        1
      // POP  eax        1
      //
      length += 2;
      }

   if (action & kPreserveEDX)
      {
      // PUSH edx        1
      // POP  edx        1
      //
      length += 2;
      }

   if (action & kMOVLow)
      {
      length += 2;
      }

   if (action & kMOVHigh)
      {
      length += 2;
      }

   if (action & kXCHG)
      {
      length++;
      }

   return length + estimateRestartJumpLength(estimatedSnippetStart + length);
   }

uint8_t *TR_X86fbits2iSnippet::emitSnippetBody()
   {
   // *this    swipeable for debugging purposes
   uint8_t *buffer = cg()->getBinaryBufferCursor();
   getSnippetLabel()->setCodeLocation(buffer);
   *buffer++ = 0xf7;
   *buffer = 0xc0;
   getRegister()->setRegisterFieldInModRM(buffer++);
   *(int32_t *)buffer = 0xfffffe00;  // test R, 0xfffffe00
   buffer += 4;
   uint8_t *location = _instruction->getBinaryEncoding();
   int32_t  distance = location - (buffer + 2);
   if (distance < -128 || distance > 127) // long jump to rol instruction
      {
      *(uint16_t *)buffer = 0x840f;
      buffer += 2;
      *(uint32_t *)buffer = (uint32_t)(distance - 4);
      buffer += 4;
      }
   else // short jump to rol instruction
      {
      *buffer++ = 0x74;
      *buffer++ = (uint8_t)distance;
      }
   *buffer = 0xb8;
   getRegister()->setRegisterFieldInModRM(buffer++);
   *(uint32_t *)buffer = FLOAT_NAN;  // mov   R, FLOAT_NAN

   return genRestartJump(buffer+4);
   }


void
TR_Debug::print(TR::FILE *pOutFile, TR_X86fbits2iSnippet  * snippet)
   {
   // *this    swipeable for debugging purposes

   if (pOutFile == NULL)
      return;

   uint8_t *bufferPos = snippet->getSnippetLabel()->getCodeLocation();

   printSnippetLabel(pOutFile, snippet->getSnippetLabel(), bufferPos, getName(snippet));

   printPrefix(pOutFile, NULL, bufferPos, 6);
   trfprintf(pOutFile, "test\t%s, %sfffffe00%s",
                 getName(snippet->getRegister()),
                 hexPrefixString(),
                 hexSuffixString());
   bufferPos += 6;

   int32_t size;
   if (*bufferPos == 0x74)
      size = 2;
   else
      size = 6;
   printPrefix(pOutFile, NULL, bufferPos, size);
   printLabelInstruction(pOutFile, "je", snippet->getRestartLabel());
   print(pOutFile, snippet->getRestartLabel());
   trfprintf(pOutFile, " - 3");
   bufferPos += size;

   printPrefix(pOutFile, NULL, bufferPos, 5);
   trfprintf(pOutFile, "mov\t%s, " POINTER_PRINTF_FORMAT, getName(snippet->getRegister()), FLOAT_NAN);
   bufferPos += 5;

   printRestartJump(pOutFile, snippet, bufferPos);
   }


uint32_t TR_X86fbits2iSnippet::getLength(int32_t estimatedSnippetStart)
   {
   // *this    swipeable for debugging purposes
   uint32_t length = 6;  // 6 for test instruction

   int32_t location = getRestartLabel()->getEstimatedCodeLocation();
   if (getRestartLabel()->getCodeLocation() != 0)
      {
      location = getRestartLabel()->getCodeLocation() - cg()->getBinaryBufferStart();
      }
   int32_t distance = location - (estimatedSnippetStart + length + 2); // 2 is size of short branch
   if (distance >= -128 && distance <= 127)
      {
      length += 2; // short branch adequate
      }
   else
      {
      length += 6; // long conditional branch required
      }

   length += 5; // 5 for the mov instruction

   return length + estimateRestartJumpLength(estimatedSnippetStart + length);
   }

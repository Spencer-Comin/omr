/*******************************************************************************
 * Copyright IBM Corp. and others 2020
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "optimizer/abstractinterpreter/IDTBuilder.hpp"
#include "optimizer/abstractinterpreter/IDT.hpp"
#include "il/Block.hpp"
#ifdef J9_PROJECT_SPECIFIC
#include "env/j9method.h"
#include "control/RecompilationInfo.hpp"
#endif

#define COLD_ROOT_CALL_RATIO 0.5

OMR::IDTBuilder::IDTBuilder(TR::ResolvedMethodSymbol* symbol, int32_t budget, TR::Region& region, TR::Compilation* comp, TR_InlinerBase* inliner) :
      _idt(NULL),
      _rootSymbol(symbol),
      _rootBudget(budget),
      _region(region),
      _comp(comp),
      _inliner(inliner)
   {}

TR::IDTBuilder* OMR::IDTBuilder::self()
   {
   return static_cast<TR::IDTBuilder*>(this);
   }

TR::IDT* OMR::IDTBuilder::buildIDT()
   {
   bool traceBIIDTGen = comp()->getOption(TR_TraceBIIDTGen);

   if (traceBIIDTGen)
      traceMsg(comp(), "\n+ IDTBuilder: Start building IDT |\n\n");

   TR_ResolvedMethod* rootMethod = _rootSymbol->getResolvedMethod();
   TR_ByteCodeInfo bcInfo;
   bcInfo.setInvalidCallerIndex();
   bcInfo.setInvalidByteCodeIndex();
   bcInfo.setDoNotProfile(true);

   //This is just a fake callsite to make ECS work.
   TR_CallSite *rootCallSite = new (region()) TR_CallSite(
                                                rootMethod,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL,
                                                rootMethod->containingClass(),
                                                0,
                                                0,
                                                rootMethod,
                                                _rootSymbol,
                                                _rootSymbol->getMethodKind() == TR::MethodSymbol::Kinds::Virtual || _rootSymbol->getMethodKind() == TR::MethodSymbol::Kinds::Interface ,
                                                _rootSymbol->getMethodKind() == TR::MethodSymbol::Kinds::Interface,
                                                bcInfo,
                                                comp(),
                                                -1,
                                                false);

   TR_CallTarget *rootCallTarget = new (region()) TR_CallTarget(
                                    region(),
                                    rootCallSite,
                                    _rootSymbol,
                                    rootMethod,
                                    NULL,
                                    rootMethod->containingClass(),
                                    NULL);

   //Initialize IDT
   _idt = new (region()) TR::IDT(region(), rootCallTarget, _rootSymbol, _rootBudget, comp());

   TR::IDTNode* root = _idt->getRoot();

   //generate the CFG for root call target
   TR::CFG* cfg = self()->generateControlFlowGraph(rootCallTarget);

   if (!cfg) //Fail to generate a CFG
      return _idt;

   //add the IDT decendants
   buildIDT2(root, NULL, _rootBudget, NULL);

   if (traceBIIDTGen)
      traceMsg(comp(), "\n+ IDTBuilder: Finish building TR::IDT |\n");

   return _idt;
   }

void OMR::IDTBuilder::buildIDT2(TR::IDTNode* node, TR::vector<TR::AbsValue*, TR::Region&>* arguments, int32_t budget, TR_CallStack* callStack)
   {
   TR::ResolvedMethodSymbol* symbol = node->getResolvedMethodSymbol();
   TR_ResolvedMethod* method = node->getResolvedMethod();

   TR_CallStack* nextCallStack = new (region()) TR_CallStack(comp(), symbol, method, callStack, budget, true);

   // Abstract interpretation will identify and find callsites thus they will be added to the IDT
   OMR::IDTBuilder::Visitor visitor(self(), node, nextCallStack);

   self()->performAbstractInterpretation(node, visitor, arguments);

   // At this point we have the inlining summary generated by abstract interpretation
   // So we can use the summary and the arguments passed from callers to calculate the static benefit.
   if (!node->isRoot())
      {
      uint32_t staticBenefit = computeStaticBenefit(node->getInliningMethodSummary(), arguments);
      node->setStaticBenefit(staticBenefit);
      }
   }

void OMR::IDTBuilder::addNodesToIDT(TR::IDTNode*parent, TR_CallSite* callSite, float callRatio, TR::vector<TR::AbsValue*, TR::Region&>* arguments, TR_CallStack* callStack)
   {
   bool traceBIIDTGen = comp()->getOption(TR_TraceBIIDTGen);

   if (callSite == NULL)
      {
      if (traceBIIDTGen)
         traceMsg(comp(), "Do not have a callsite. Don't add\n");
      return;
      }

   if (traceBIIDTGen)
      traceMsg(comp(), "+ IDTBuilder: Adding a child Node: %s for TR::IDTNode: %s\n", callSite->signature(comp()->trMemory()), parent->getName(comp()->trMemory()));

   callSite->findCallSiteTarget(callStack, getInliner()); //Find all call targets

   // eliminate call targets that are not inlinable according to the policy
   // thus they won't be added to IDT
   getInliner()->applyPolicyToTargets(callStack, callSite);

   if (callSite->numTargets() == 0)
      {
      if (traceBIIDTGen)
         traceMsg(comp(), "Do not have a call target. Don't add\n");
      return;
      }

   for (int32_t i = 0 ; i < callSite->numTargets(); i++)
      {
      TR_CallTarget* callTarget = callSite->getTarget(i);

      int32_t remainingBudget = parent->getBudget() - callTarget->_calleeMethod->maxBytecodeIndex();

      if (remainingBudget < 0) // no budget remains
         {
         if (traceBIIDTGen)
            traceMsg(comp(), "No budget left. Don't add\n");
         continue;
         }

      bool isRecursiveCall = callStack->isAnywhereOnTheStack(callTarget->_calleeMethod, 1);

      if (isRecursiveCall) //Stop for recursive call
         {
         if (traceBIIDTGen)
            traceMsg(comp(), "Recursive call. Don't add\n");
         continue;
         }

      if (parent->getRootCallRatio() * callRatio * callTarget->_frequencyAdjustment < COLD_ROOT_CALL_RATIO)
         continue;

#ifdef J9_PROJECT_SPECIFIC
      // hot and scorching bodies should never be inlined to warm or cooler bodies
      if (!callTarget->_calleeMethod->isInterpreted())
         {
         TR_PersistentJittedBodyInfo * bodyInfo = ((TR_ResolvedJ9Method*) callTarget->_calleeMethod)->getExistingJittedBodyInfo();
         if (bodyInfo && comp()->getMethodHotness() <= warm && bodyInfo->getHotness() >= hot)
            continue;
         }
#endif

      // generate the CFG of this call target and set the block frequencies.
      TR::CFG* cfg = self()->generateControlFlowGraph(callTarget);

      if (!cfg)
         {
         if (traceBIIDTGen)
            traceMsg(comp(), "Fail to generate a CFG. Don't add\n");
         continue;
         }

      // The actual symbol for the callTarget->_calleeMethod.
      TR::ResolvedMethodSymbol* calleeMethodSymbol = TR::ResolvedMethodSymbol::create(comp()->trHeapMemory(), callTarget->_calleeMethod, comp());

      TR::IDTNode* child = parent->addChild(
                              _idt->getNextGlobalIDTNodeIndex(),
                              callTarget,
                              calleeMethodSymbol,
                              callSite->_byteCodeIndex,
                              callRatio * callTarget->_frequencyAdjustment,
                              _idt->getRegion()
                              );

      _idt->increaseGlobalIDTNodeIndex();
      _idt->addCost(child->getCost());

      if (!comp()->incInlineDepth(calleeMethodSymbol, callSite->_bcInfo, callSite->_cpIndex, NULL, !callSite->isIndirectCall(), 0))
         continue;

      // Build the IDT recursively
      buildIDT2(child, arguments, child->getBudget(), callStack);

      comp()->decInlineDepth(true);
      }
   }

uint32_t OMR::IDTBuilder::computeStaticBenefit(TR::InliningMethodSummary* summary, TR::vector<TR::AbsValue*, TR::Region&>* arguments)
   {
   if (summary == NULL || arguments == NULL)
      return 0;

   uint32_t staticBenefit = 0;

   for (size_t i = 0; i < arguments->size(); i ++)
      {
      TR::AbsValue* arg = arguments->at(i);
      staticBenefit += summary->testArgument(arg, static_cast<uint32_t>(i));
      }

   return staticBenefit;
   }

void OMR::IDTBuilder::Visitor::visitCallSite(TR_CallSite* callSite, TR::Block* callBlock, TR::vector<TR::AbsValue*, TR::Region&>* arguments)
   {
   float callRatio = (float)callBlock->getFrequency() / (float)_idtNode->getCallTarget()->_cfg->getStart()->asBlock()->getFrequency();

   if (callBlock->getFrequency() < 6 || callBlock->isCold() || callBlock->isSuperCold())
      return;

   _idtBuilder->addNodesToIDT(_idtNode, callSite, callRatio, arguments, _callStack);
   }

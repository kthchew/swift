//===--- LoopUtils.cpp ----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-loop-utils"
#include "swift/SILOptimizer/Utils/LoopUtils.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/Support/Debug.h"

using namespace swift;

static SILBasicBlock *createInitialPreheader(SILBasicBlock *Header) {
  auto *Preheader =
      Header->getParent()->createBasicBlockBefore(Header);

  // Clone the arguments from header into the pre-header.
  llvm::SmallVector<SILValue, 8> Args;
  for (auto *HeaderArg : Header->getArguments()) {
    Args.push_back(Preheader->createPhiArgument(HeaderArg->getType(),
                                                ValueOwnershipKind::Owned));
  }

  // Create the branch to the header.
  SILBuilder(Preheader).createBranch(
      RegularLocation::getAutoGeneratedLocation(), Header, Args);

  return Preheader;
}

/// Create a unique loop preheader.
static SILBasicBlock *insertPreheader(SILLoop *L, DominanceInfo *DT,
                                      SILLoopInfo *LI) {
  assert(!L->getLoopPreheader() && "Expect multiple preheaders");
  SILBasicBlock *Header = L->getHeader();

  // Before we create the preheader, gather all of the original preds of header.
  llvm::SmallVector<SILBasicBlock *, 8> Preds;
  for (auto *Pred : Header->getPredecessorBlocks()) {
    if (!L->contains(Pred)) {
      Preds.push_back(Pred);
    }
  }

  // Then create the pre-header and connect it to header.
  SILBasicBlock *Preheader = createInitialPreheader(Header);

  // Then change all of the original predecessors to target Preheader instead of
  // header.
  for (auto *Pred : Preds) {
    replaceBranchTarget(Pred->getTerminator(), Header, Preheader,
                        true /*PreserveArgs*/);
  }

  // Update dominance info.
  if (DT) {
    // Get the dominance node of the header.
    auto *HeaderBBDTNode = DT->getNode(Header);
    if (HeaderBBDTNode) {
      // Make a DTNode for the preheader and make the header's immediate
      // dominator, the immediate dominator of the pre-header.
      auto *PreheaderDTNode =
          DT->addNewBlock(Preheader, HeaderBBDTNode->getIDom()->getBlock());
      // Then change the immediate dominator of the header to be the pre-header.
      HeaderBBDTNode->setIDom(PreheaderDTNode);
    }
  }

  // Make the pre-header a part of the parent loop of L if L has a parent loop.
  if (LI) {
    if (auto *PLoop = L->getParentLoop())
      PLoop->addBasicBlockToLoop(Preheader, LI->getBase());
  }

  return Preheader;
}

/// Convert a loop with multiple backedges to a single backedge loop.
///
/// Create a new block as a common target for all the current loop backedges.
static SILBasicBlock *insertBackedgeBlock(SILLoop *L, DominanceInfo *DT,
                                          SILLoopInfo *LI) {
  assert(!L->getLoopLatch() && "Must have > 1 backedge.");

  // For simplicity, assume a single preheader
  SILBasicBlock *Preheader = L->getLoopPreheader();
  assert(Preheader && "A preheader should have been created before calling"
         "this function");

  SILBasicBlock *Header = L->getHeader();
  SILFunction *F = Header->getParent();

  // Figure out which basic blocks contain back-edges to the loop header.
  SmallVector<SILBasicBlock*, 4> BackedgeBlocks;
  for (auto *Pred : Header->getPredecessorBlocks()) {
    if (Pred == Preheader)
      continue;
    // Branches can be handled trivially and CondBranch edges can be split.
    if (!isa<BranchInst>(Pred->getTerminator())
        && !isa<CondBranchInst>(Pred->getTerminator())) {
      return nullptr;
    }
    BackedgeBlocks.push_back(Pred);
  }

  // Create and insert the new backedge block...
  SILBasicBlock *BEBlock = F->createBasicBlockAfter(BackedgeBlocks.back());

  LLVM_DEBUG(llvm::dbgs() << "  Inserting unique backedge block " << *BEBlock
                          << "\n");

  // Now that the block has been inserted into the function, create PHI nodes in
  // the backedge block which correspond to any PHI nodes in the header block.
  SmallVector<SILValue, 6> BBArgs;
  for (auto *BBArg : Header->getArguments()) {
    BBArgs.push_back(BEBlock->createPhiArgument(BBArg->getType(),
                                                ValueOwnershipKind::Owned));
  }

  // Arbitrarily pick one of the predecessor's branch locations.
  SILLocation BranchLoc = BackedgeBlocks.back()->getTerminator()->getLoc();

  // Create an unconditional branch that propagates the newly created BBArgs.
  SILBuilder(BEBlock).createBranch(BranchLoc, Header, BBArgs);

  // Redirect the backedge blocks to BEBlock instead of Header.
  for (auto *Pred : BackedgeBlocks) {
    auto *Terminator = Pred->getTerminator();

    if (auto *Branch = dyn_cast<BranchInst>(Terminator))
      changeBranchTarget(Branch, 0, BEBlock, /*PreserveArgs=*/true);
    else if (auto *CondBranch = dyn_cast<CondBranchInst>(Terminator)) {
      unsigned EdgeIdx = (CondBranch->getTrueBB() == Header)
        ? CondBranchInst::TrueIdx : CondBranchInst::FalseIdx;
      changeBranchTarget(CondBranch, EdgeIdx, BEBlock, /*PreserveArgs=*/true);
    }
    else {
      llvm_unreachable("Expected a branch terminator.");
    }
  }

  // Update Loop Information - we know that this block is now in the current
  // loop and all parent loops.
  L->addBasicBlockToLoop(BEBlock, LI->getBase());

  // Update dominator information
  SILBasicBlock *DomBB = BackedgeBlocks.back();
  for (auto BBIter = BackedgeBlocks.begin(),
         BBEnd = std::prev(BackedgeBlocks.end());
       BBIter != BBEnd; ++BBIter) {
    DomBB = DT->findNearestCommonDominator(DomBB, *BBIter);
  }
  DT->addNewBlock(BEBlock, DomBB);

  return BEBlock;
}

/// Canonicalize the loop for rotation and downstream passes.
///
/// Create a single preheader and single latch block.
///
/// FIXME: We should identify nested loops with a common header and separate
/// them before merging the latch. See LLVM's separateNestedLoop.
bool swift::canonicalizeLoop(SILLoop *L, DominanceInfo *DT, SILLoopInfo *LI) {
  bool ChangedCFG = false;

  if (!L->getLoopPreheader()) {
    insertPreheader(L, DT, LI);
    assert(L->getLoopPreheader() && "L should have a pre-header now");
    ChangedCFG = true;
  }
  if (!L->getLoopLatch())
    ChangedCFG |= (insertBackedgeBlock(L, DT, LI) != nullptr);

  return ChangedCFG;
}

bool swift::canonicalizeAllLoops(DominanceInfo *DT, SILLoopInfo *LI) {
  // Visit the loop nest hierarchy bottom up.
  bool MadeChange = false;
  llvm::SmallVector<std::pair<SILLoop *, bool>, 16> Worklist;
  for (auto *L : LI->getTopLevelLoops())
    Worklist.push_back({L, L->empty()});

  while (Worklist.size()) {
    SILLoop *L;
    bool VisitedAlready;
    std::tie(L, VisitedAlready) = Worklist.pop_back_val();

    if (!VisitedAlready) {
      Worklist.push_back({L, true});
      for (auto *Subloop : L->getSubLoopRange()) {
        Worklist.push_back({Subloop, Subloop->empty()});
      }
      continue;
    }

    MadeChange |= canonicalizeLoop(L, DT, LI);
  }

  return MadeChange;
}

//===----------------------------------------------------------------------===//
//                                Loop Visitor
//===----------------------------------------------------------------------===//

void SILLoopVisitor::run() {
  // We visit the loop nest inside out via a depth first, post order using
  // this
  // worklist.
  llvm::SmallVector<std::pair<SILLoop *, bool>, 32> Worklist;
  for (auto *L : LI->getTopLevelLoops()) {
    Worklist.push_back({L, L->empty()});
  }

  while (Worklist.size()) {
    SILLoop *L;
    bool Visited;
    std::tie(L, Visited) = Worklist.pop_back_val();

    if (!Visited) {
      Worklist.push_back({L, true});
      for (auto *SubLoop : L->getSubLoops()) {
        Worklist.push_back({SubLoop, SubLoop->empty()});
      }
      continue;
    }
    runOnLoop(L);
  }

  runOnFunction(F);
}

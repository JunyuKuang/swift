//===--- TempRValueElimination.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Eliminate temporary RValues inserted as a result of materialization by
/// SILGen. The key pattern here is that we are looking for alloc_stack that are
/// only written to once and are eventually either destroyed/taken from.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-temp-rvalue-opt"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                 Interface
//===----------------------------------------------------------------------===//

namespace {

/// Temporary RValue Optimization
///
/// Peephole optimization to eliminate short-lived immutable temporary copies.
/// This handles a common pattern generated by SILGen where temporary RValues
/// are emitted as copies...
///
///   %temp = alloc_stack $T
///   copy_addr %src to [initialization] %temp : $*T
///   // no writes to %src or %temp
///   destroy_addr %temp : $*T
///   dealloc_stack %temp : $*T
///
/// This differs from the copy forwarding algorithm because it handles
/// copy source and dest lifetimes that are unavoidably overlappying. Instead,
/// it finds cases in which it is easy to determine that the source is
/// unmodified during the copy destination's lifetime. Thus, the destination can
/// be viewed as a short-lived "rvalue".
class TempRValueOptPass : public SILFunctionTransform {
  AliasAnalysis *aa = nullptr;

  bool collectLoads(Operand *userOp, SILInstruction *userInst,
                    SingleValueInstruction *addr, SILValue srcObject,
                    SmallPtrSetImpl<SILInstruction *> &loadInsts);
  bool collectLoadsFromProjection(SingleValueInstruction *projection,
                                  SILValue srcAddr,
                                  SmallPtrSetImpl<SILInstruction *> &loadInsts);

  bool
  checkNoSourceModification(CopyAddrInst *copyInst, SILValue copySrc,
                            const SmallPtrSetImpl<SILInstruction *> &useInsts);

  bool
  checkTempObjectDestroy(AllocStackInst *tempObj, CopyAddrInst *copyInst,
                         ValueLifetimeAnalysis::Frontier &tempAddressFrontier);

  bool tryOptimizeCopyIntoTemp(CopyAddrInst *copyInst);
  std::pair<SILBasicBlock::iterator, bool>
  tryOptimizeStoreIntoTemp(StoreInst *si);

  void run() override;
};

} // anonymous namespace

bool TempRValueOptPass::collectLoadsFromProjection(
    SingleValueInstruction *projection, SILValue srcAddr,
    SmallPtrSetImpl<SILInstruction *> &loadInsts) {
  if (!srcAddr) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "  Temp has addr_projection use?! Can not yet promote to value"
        << *projection);
    return false;
  }

  // Transitively look through projections on stack addresses.
  for (auto *projUseOper : projection->getUses()) {
    auto *user = projUseOper->getUser();
    if (user->isTypeDependentOperand(*projUseOper))
      continue;

    if (!collectLoads(projUseOper, user, projection, srcAddr, loadInsts))
      return false;
  }
  return true;
}

/// Transitively explore all data flow uses of the given \p address until
/// reaching a load or returning false.
///
/// Any user opcode recognized by collectLoads must be replaced correctly later
/// during tryOptimizeCopyIntoTemp. If it is possible for any use to destroy the
/// value in \p address, then that use must be removed or made non-destructive
/// after the copy is removed and its operand is replaced.
///
/// Warning: To preserve the original object lifetime, tryOptimizeCopyIntoTemp
/// must assume that there are no holes in lifetime of the temporary stack
/// location at \address. The temporary must be initialized by the original copy
/// and never written to again. Therefore, collectLoads disallows any operation
/// that may write to memory at \p address.
bool TempRValueOptPass::collectLoads(
    Operand *userOp, SILInstruction *user, SingleValueInstruction *address,
    SILValue srcAddr, SmallPtrSetImpl<SILInstruction *> &loadInsts) {
  // All normal uses (loads) must be in the initialization block.
  // (The destroy and dealloc are commonly in a different block though.)
  if (user->getParent() != address->getParent())
    return false;

  // Only allow uses that cannot destroy their operand. We need to be sure
  // that replacing all this temporary's uses with the copy source doesn't
  // destroy the source. This way, we know that the destroy_addr instructions
  // that we recorded cover all the temporary's lifetime termination points.
  //
  // Currently this includes address projections, loads, and in_guaranteed uses
  // by an apply.
  //
  // TODO: handle non-destructive projections of enums
  // (unchecked_take_enum_data_addr of Optional is nondestructive.)
  switch (user->getKind()) {
  default:
    LLVM_DEBUG(llvm::dbgs()
               << "  Temp use may write/destroy its source" << *user);
    return false;

  case SILInstructionKind::BeginAccessInst:
    return cast<BeginAccessInst>(user)->getAccessKind() == SILAccessKind::Read;

  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::TryApplyInst: {
    ApplySite apply(user);

    // Check if the function can just read from userOp.
    auto convention = apply.getArgumentConvention(*userOp);
    if (!convention.isGuaranteedConvention()) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp consuming use may write/destroy "
                                 "its source"
                              << *user);
      return false;
    }

    // If we do not have an src address, but are indirect, bail. We would need
    // to perform function signature specialization to change the functions
    // signature to pass something direct.
    if (!srcAddr && convention.isIndirectConvention()) {
      LLVM_DEBUG(
          llvm::dbgs()
          << "  Temp used to materialize value for indirect convention?! Can "
             "not remove temporary without func sig opts"
          << *user);
      return false;
    }

    // Check if there is another function argument, which is inout which might
    // modify the source address if we have one.
    //
    // When a use of the temporary is an apply, then we need to prove that the
    // function called by the apply cannot modify the temporary's source
    // value. By design, this should be handled by
    // `checkNoSourceModification`. However, this would be too conservative
    // since it's common for the apply to have an @out argument, and alias
    // analysis cannot prove that the @out does not alias with `src`. Instead,
    // `checkNoSourceModification` always avoids analyzing the current use, so
    // applies need to be handled here. We already know that an @out cannot
    // alias with `src` because the `src` value must be initialized at the point
    // of the call. Hence, it is sufficient to check specifically for another
    // @inout that might alias with `src`.
    if (srcAddr) {
      auto calleeConv = apply.getSubstCalleeConv();
      unsigned calleeArgIdx = apply.getCalleeArgIndexOfFirstAppliedArg();
      for (const auto &operand : apply.getArgumentOperands()) {
        auto argConv = calleeConv.getSILArgumentConvention(calleeArgIdx);
        if (argConv.isInoutConvention()) {
          if (!aa->isNoAlias(operand.get(), srcAddr)) {
            return false;
          }
        }
        ++calleeArgIdx;
      }
    }

    // Everything is okay with the function call. Register it as a "load".
    loadInsts.insert(user);
    return true;
  }
  case SILInstructionKind::OpenExistentialAddrInst: {
    // If we do not have an srcAddr, bail. We do not support promoting this yet.
    if (!srcAddr) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp has open_existential_addr use?! Can "
                                 "not yet promote to value"
                              << *user);
      return false;
    }

    // We only support open existential addr if the access is immutable.
    auto *oeai = cast<OpenExistentialAddrInst>(user);
    if (oeai->getAccessKind() != OpenedExistentialAccess::Immutable) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp consuming use may write/destroy "
                                 "its source"
                              << *user);
      return false;
    }
    return collectLoadsFromProjection(oeai, srcAddr, loadInsts);
  }
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
    // In certain cases, unchecked_take_enum_data_addr invalidates the
    // underlying memory, so by default we can not look through it... but this
    // is not true in the case of Optional. This is an important case for us to
    // handle, so handle it here.
    auto *utedai = cast<UncheckedTakeEnumDataAddrInst>(user);
    if (!utedai->getOperand()->getType().getOptionalObjectType()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  Temp use may write/destroy its source" << *utedai);
      return false;
    }

    return collectLoadsFromProjection(utedai, srcAddr, loadInsts);
  }
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst: {
    return collectLoadsFromProjection(cast<SingleValueInstruction>(user),
                                      srcAddr, loadInsts);
  }
  case SILInstructionKind::LoadInst:
    // Loads are the end of the data flow chain. The users of the load can't
    // access the temporary storage.
    //
    // That being said, if we see a load [take] here then we must have had a
    // load [take] of a projection of our temporary stack location since we skip
    // all the load [take] of the top level allocation in the caller of this
    // function. So if we have such a load [take], we /must/ have a
    // reinitialization or an alloc_stack that does not fit the pattern we are
    // expecting from SILGen. Be conservative and return false.
    if (auto *li = dyn_cast<LoadInst>(user)) {
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        return false;
      }
    }
    loadInsts.insert(user);
    return true;

  case SILInstructionKind::LoadBorrowInst:
    // If we do not have a source addr, we must be trying to eliminate a
    // store. Until we check that the source object is not destroyed within the
    // given range, we need bail.
    if (!srcAddr)
      return false;
    loadInsts.insert(user);
    return true;
  case SILInstructionKind::FixLifetimeInst:
    // If we have a fixed lifetime on our alloc_stack, we can just treat it like
    // a load and re-write it so that it is on the old memory or old src object.
    loadInsts.insert(user);
    return true;
  case SILInstructionKind::CopyAddrInst: {
    // copy_addr which read from the temporary are like loads.
    auto *copyFromTmp = cast<CopyAddrInst>(user);
    if (copyFromTmp->getDest() == address) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp written or taken" << *user);
      return false;
    }
    loadInsts.insert(copyFromTmp);
    return true;
  }
  }
}

/// Checks if the copy's source can be modified within the temporary's lifetime.
///
/// Unfortunately, we cannot simply use the destroy points as the lifetime end,
/// because they can be in a different basic block (that's what SILGen
/// generates). Instead we guarantee that all normal uses are within the block
/// of the temporary and look for the last use, which effectively ends the
/// lifetime.
bool TempRValueOptPass::checkNoSourceModification(
    CopyAddrInst *copyInst, SILValue copySrc,
    const SmallPtrSetImpl<SILInstruction *> &useInsts) {
  unsigned numLoadsFound = 0;
  auto iter = std::next(copyInst->getIterator());
  // We already checked that the useful lifetime of the temporary ends in
  // the initialization block.
  auto iterEnd = copyInst->getParent()->end();
  for (; iter != iterEnd; ++iter) {
    SILInstruction *inst = &*iter;

    if (useInsts.count(inst))
      numLoadsFound++;

    // If this is the last use of the temp we are ok. After this point,
    // modifications to the source don't matter anymore.
    if (numLoadsFound == useInsts.size())
      return true;

    if (aa->mayWriteToMemory(inst, copySrc)) {
      LLVM_DEBUG(llvm::dbgs() << "  Source modified by" << *iter);
      return false;
    }
  }
  // For some reason, not all normal uses have been seen between the copy and
  // the end of the initialization block. We should never reach here.
  return false;
}

/// Return true if the \p tempObj, which is initialized by \p copyInst, is
/// destroyed in an orthodox way.
///
/// When tryOptimizeCopyIntoTemp replaces all of tempObj's uses, it assumes that
/// the object is initialized by the original copy and directly destroyed on all
/// paths by one of the recognized 'destroy_addr' or 'copy_addr [take]'
/// operations. This assumption must be checked. For example, in non-OSSA,
/// it is legal to destroy an in-memory object by loading the value and
/// releasing it. Rather than detecting unbalanced load releases, simply check
/// that tempObj is destroyed directly on all paths.
bool TempRValueOptPass::checkTempObjectDestroy(
    AllocStackInst *tempObj, CopyAddrInst *copyInst,
    ValueLifetimeAnalysis::Frontier &tempAddressFrontier) {
  // If the original copy was a take, then replacing all uses cannot affect
  // the lifetime.
  if (copyInst->isTakeOfSrc())
    return true;

  // ValueLifetimeAnalysis is not normally used for address types. It does not
  // reason about the lifetime of the in-memory object. However the utility can
  // be abused here to check that the address is directly destroyed on all
  // paths. collectLoads has already guaranteed that tempObj's lifetime has no
  // holes/reinitializations.
  SmallVector<SILInstruction *, 8> users;
  for (auto result : tempObj->getResults()) {
    for (Operand *operand : result->getUses()) {
      SILInstruction *user = operand->getUser();
      if (user == copyInst)
        continue;
      if (isa<DeallocStackInst>(user))
        continue;
      users.push_back(user);
    }
  }
  // Find the boundary of tempObj's address lifetime, starting at copyInst.
  ValueLifetimeAnalysis vla(copyInst, users);
  if (!vla.computeFrontier(tempAddressFrontier,
                           ValueLifetimeAnalysis::DontModifyCFG)) {
    return false;
  }
  // Check that the lifetime boundary ends at direct destroy points.
  for (SILInstruction *frontierInst : tempAddressFrontier) {
    auto pos = frontierInst->getIterator();
    // If the frontier is at the head of a block, then either it is an
    // unexpected lifetime exit, or the lifetime ended at a
    // terminator. TempRValueOptPass does not handle either case.
    if (pos == frontierInst->getParent()->begin())
      return false;

    // Look for a known destroy point as described in the function level
    // comment. This whitelist can be expanded as more cases are handled in
    // tryOptimizeCopyIntoTemp during copy replacement.
    SILInstruction *lastUser = &*std::prev(pos);
    if (isa<DestroyAddrInst>(lastUser))
      continue;

    if (auto *li = dyn_cast<LoadInst>(lastUser)) {
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        continue;
      }
    }

    if (auto *cai = dyn_cast<CopyAddrInst>(lastUser)) {
      assert(cai->getSrc() == tempObj && "collectLoads checks for writes");
      assert(!copyInst->isTakeOfSrc() && "checked above");
      if (cai->isTakeOfSrc())
        continue;
    }
    return false;
  }
  return true;
}

/// Tries to perform the temporary rvalue copy elimination for \p copyInst
bool TempRValueOptPass::tryOptimizeCopyIntoTemp(CopyAddrInst *copyInst) {
  if (!copyInst->isInitializationOfDest())
    return false;

  auto *tempObj = dyn_cast<AllocStackInst>(copyInst->getDest());
  if (!tempObj)
    return false;

  // The copy's source address must not be a scoped instruction, like
  // begin_borrow. When the temporary object is eliminated, it's uses are
  // replaced with the copy's source. Therefore, the source address must be
  // valid at least until the next instruction that may write to or destroy the
  // source. End-of-scope markers, such as end_borrow, do not write to or
  // destroy memory, so scoped addresses are not valid replacements.
  SILValue copySrc = stripAccessMarkers(copyInst->getSrc());

  assert(tempObj != copySrc && "can't initialize temporary with itself");

  // Scan all uses of the temporary storage (tempObj) to verify they all refer
  // to the value initialized by this copy. It is sufficient to check that the
  // only users that modify memory are the copy_addr [initialization] and
  // destroy_addr.
  SmallPtrSet<SILInstruction *, 8> loadInsts;
  for (auto *useOper : tempObj->getUses()) {
    SILInstruction *user = useOper->getUser();

    if (user == copyInst)
      continue;

    // Destroys and deallocations are allowed to be in a different block.
    if (isa<DestroyAddrInst>(user) || isa<DeallocStackInst>(user))
      continue;

    // Same for load [take] on the top level temp object. SILGen always takes
    // whole values from temporaries. If we have load [take] on projections from
    // our base, we fail since those would be re-initializations.
    if (auto *li = dyn_cast<LoadInst>(user)) {
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        continue;
      }
    }

    if (!collectLoads(useOper, user, tempObj, copySrc, loadInsts))
      return false;
  }

  // Check if the source is modified within the lifetime of the temporary.
  if (!checkNoSourceModification(copyInst, copySrc, loadInsts))
    return false;

  ValueLifetimeAnalysis::Frontier tempAddressFrontier;
  if (!checkTempObjectDestroy(tempObj, copyInst, tempAddressFrontier))
    return false;

  LLVM_DEBUG(llvm::dbgs() << "  Success: replace temp" << *tempObj);

  // Do a "replaceAllUses" by either deleting the users or replacing them with
  // the source address. Note: we must not delete the original copyInst because
  // it would crash the instruction iteration in run(). Instead the copyInst
  // gets identical Src and Dest operands.
  //
  // NOTE: We delete instructions at the end to allow us to use
  // tempAddressFrontier to insert compensating destroys for load [take].
  SmallVector<SILInstruction *, 4> toDelete;
  while (!tempObj->use_empty()) {
    Operand *use = *tempObj->use_begin();
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::DestroyAddrInst:
      if (copyInst->isTakeOfSrc()) {
        use->set(copySrc);
      } else {
        user->dropAllReferences();
        toDelete.push_back(user);
      }
      break;
    case SILInstructionKind::DeallocStackInst:
      user->dropAllReferences();
      toDelete.push_back(user);
      break;
    case SILInstructionKind::CopyAddrInst: {
      auto *cai = cast<CopyAddrInst>(user);
      if (cai != copyInst) {
        assert(cai->getSrc() == tempObj);
        if (cai->isTakeOfSrc() && !copyInst->isTakeOfSrc())
          cai->setIsTakeOfSrc(IsNotTake);
      }
      use->set(copySrc);
      break;
    }
    case SILInstructionKind::LoadInst: {
      // If we do not have a load [take] or we have a load [take] and our
      // copy_addr takes the source, just do the normal thing of setting the
      // load to use the copyInst's source.
      auto *li = cast<LoadInst>(user);
      if (li->getOwnershipQualifier() != LoadOwnershipQualifier::Take ||
          copyInst->isTakeOfSrc()) {
        use->set(copyInst->getSrc());
        break;
      }

      // Otherwise, since copy_addr is not taking src, we need to ensure that we
      // insert a copy of our value. We do that by creating a load [copy] at the
      // copy_addr inst and RAUWing the load [take] with that. We then insert
      // destroy_value for the load [copy] at all points where we had destroys
      // that are not the specific take that we were optimizing.
      SILBuilderWithScope builder(copyInst);
      SILValue newLoad = builder.emitLoadValueOperation(
          copyInst->getLoc(), copyInst->getSrc(), LoadOwnershipQualifier::Copy);
      for (auto *inst : tempAddressFrontier) {
        assert(inst->getIterator() != inst->getParent()->begin() &&
               "Should have caught this when checking destructor");
        auto prevInst = std::prev(inst->getIterator());
        if (&*prevInst == li)
          continue;
        SILBuilderWithScope builder(prevInst);
        builder.emitDestroyValueOperation(prevInst->getLoc(), newLoad);
      }
      li->replaceAllUsesWith(newLoad);
      li->dropAllReferences();
      toDelete.push_back(li);
      break;
    }

    // ASSUMPTION: no operations that may be handled by this default clause can
    // destroy tempObj. This includes operations that load the value from memory
    // and release it or cast the address before destroying it.
    default:
      use->set(copySrc);
      break;
    }
  }

  while (!toDelete.empty()) {
    toDelete.pop_back_val()->eraseFromParent();
  }
  tempObj->eraseFromParent();
  return true;
}

std::pair<SILBasicBlock::iterator, bool>
TempRValueOptPass::tryOptimizeStoreIntoTemp(StoreInst *si) {
  // If our store is an assign, bail.
  if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
    return {std::next(si->getIterator()), false};

  auto *tempObj = dyn_cast<AllocStackInst>(si->getDest());
  if (!tempObj) {
    return {std::next(si->getIterator()), false};
  }

  // If our tempObj has a dynamic lifetime (meaning it is conditionally
  // initialized, conditionally taken, etc), we can not convert its uses to SSA
  // while eliminating it simply. So bail.
  if (tempObj->hasDynamicLifetime()) {
    return {std::next(si->getIterator()), false};
  }

  // Scan all uses of the temporary storage (tempObj) to verify they all refer
  // to the value initialized by this copy. It is sufficient to check that the
  // only users that modify memory are the copy_addr [initialization] and
  // destroy_addr.
  SmallPtrSet<SILInstruction *, 8> loadInsts;
  for (auto *useOper : tempObj->getUses()) {
    SILInstruction *user = useOper->getUser();

    if (user == si)
      continue;

    // Destroys and deallocations are allowed to be in a different block.
    if (isa<DestroyAddrInst>(user) || isa<DeallocStackInst>(user))
      continue;

    // Same for load [take] on the top level temp object. SILGen always takes
    // whole values from temporaries. If we have load [take] on projections from
    // our base, we fail since those would be re-initializations.
    if (auto *li = dyn_cast<LoadInst>(user)) {
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        continue;
      }
    }

    // We pass in SILValue() since we do not have a source address.
    if (!collectLoads(useOper, user, tempObj, SILValue(), loadInsts))
      return {std::next(si->getIterator()), false};
  }

  // Since store is always a consuming operation, we do not need to worry about
  // any lifetime constraints and can just replace all of the uses here. This
  // contrasts with the copy_addr implementation where we need to consider the
  // possibility that the source address is written to.
  LLVM_DEBUG(llvm::dbgs() << "  Success: replace temp" << *tempObj);

  // Do a "replaceAllUses" by either deleting the users or replacing them with
  // the appropriate operation on the source value.
  SmallVector<SILInstruction *, 4> toDelete;
  for (auto *use : tempObj->getUses()) {
    // If our store is the user, just skip it.
    if (use->getUser() == si) {
      continue;
    }

    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::DestroyAddrInst: {
      SILBuilderWithScope builder(user);
      builder.emitDestroyValueOperation(user->getLoc(), si->getSrc());
      toDelete.push_back(user);
      break;
    }
    case SILInstructionKind::DeallocStackInst:
      toDelete.push_back(user);
      break;
    case SILInstructionKind::CopyAddrInst: {
      auto *cai = cast<CopyAddrInst>(user);
      assert(cai->getSrc() == tempObj);
      SILBuilderWithScope builder(user);
      auto qualifier = cai->isInitializationOfDest()
                           ? StoreOwnershipQualifier::Init
                           : StoreOwnershipQualifier::Assign;
      SILValue src = si->getSrc();
      if (!cai->isTakeOfSrc()) {
        src = builder.emitCopyValueOperation(cai->getLoc(), src);
      }
      builder.emitStoreValueOperation(cai->getLoc(), src, cai->getDest(),
                                      qualifier);
      toDelete.push_back(cai);
      break;
    }
    case SILInstructionKind::LoadInst: {
      // Since store is always forwarding, we know that we should have our own
      // value here. So, we should be able to just RAUW any load [take] and
      // insert a copy + RAUW for any load [copy].
      auto *li = cast<LoadInst>(user);
      SILValue srcObject = si->getSrc();
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
        SILBuilderWithScope builder(li);
        srcObject = builder.emitCopyValueOperation(li->getLoc(), srcObject);
      }
      li->replaceAllUsesWith(srcObject);
      toDelete.push_back(li);
      break;
    }
    case SILInstructionKind::FixLifetimeInst: {
      auto *fli = cast<FixLifetimeInst>(user);
      SILBuilderWithScope builder(fli);
      builder.createFixLifetime(fli->getLoc(), si->getSrc());
      toDelete.push_back(fli);
      break;
    }

    // ASSUMPTION: no operations that may be handled by this default clause can
    // destroy tempObj. This includes operations that load the value from memory
    // and release it.
    default:
      llvm::errs() << "Unhandled user: " << *user;
      llvm_unreachable("Unhandled case?!");
      break;
    }
  }

  while (!toDelete.empty()) {
    auto *inst = toDelete.pop_back_val();
    inst->dropAllReferences();
    inst->eraseFromParent();
  }
  auto nextIter = std::next(si->getIterator());
  si->eraseFromParent();
  tempObj->eraseFromParent();
  return {nextIter, true};
}

//===----------------------------------------------------------------------===//
//                           High Level Entrypoint
//===----------------------------------------------------------------------===//

/// The main entry point of the pass.
void TempRValueOptPass::run() {
  LLVM_DEBUG(llvm::dbgs() << "Copy Peephole in Func "
                          << getFunction()->getName() << "\n");

  aa = getPassManager()->getAnalysis<AliasAnalysis>();
  bool changed = false;

  // Find all copy_addr instructions.
  llvm::SmallVector<CopyAddrInst *, 8> deadCopies;
  for (auto &block : *getFunction()) {
    // Increment the instruction iterator only after calling
    // tryOptimizeCopyIntoTemp because the instruction after CopyInst might be
    // deleted, but copyInst itself won't be deleted until later.
    for (auto ii = block.begin(); ii != block.end();) {
      if (auto *copyInst = dyn_cast<CopyAddrInst>(&*ii)) {
        // In case of success, this may delete instructions, but not the
        // CopyInst itself.
        changed |= tryOptimizeCopyIntoTemp(copyInst);
        // Remove identity copies which either directly result from successfully
        // calling tryOptimizeCopyIntoTemp or was created by an earlier
        // iteration, where another copy_addr copied the temporary back to the
        // source location.
        if (stripAccessMarkers(copyInst->getSrc()) == copyInst->getDest()) {
          changed = true;
          deadCopies.push_back(copyInst);
        }
        ++ii;
        continue;
      }

      if (auto *si = dyn_cast<StoreInst>(&*ii)) {
        bool madeSingleChange;
        std::tie(ii, madeSingleChange) = tryOptimizeStoreIntoTemp(si);
        changed |= madeSingleChange;
        continue;
      }

      ++ii;
    }
  }

  // Delete the copies and any unused address operands.
  // The same copy may have been added multiple times.
  sortUnique(deadCopies);
  for (auto *deadCopy : deadCopies) {
    assert(changed);
    auto *srcInst = deadCopy->getSrc()->getDefiningInstruction();
    deadCopy->eraseFromParent();
    // Simplify any access scope markers that were only used by the dead
    // copy_addr and other potentially unused addresses.
    if (srcInst) {
      if (SILValue result = simplifyInstruction(srcInst)) {
        replaceAllSimplifiedUsesAndErase(
            srcInst, result, [](SILInstruction *instToKill) {
              // SimplifyInstruction is not in the business of removing
              // copy_addr. If it were, then we would need to update deadCopies.
              assert(!isa<CopyAddrInst>(instToKill));
              instToKill->eraseFromParent();
            });
      }
    }
  }
  if (changed) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
}

SILTransform *swift::createTempRValueOpt() { return new TempRValueOptPass(); }

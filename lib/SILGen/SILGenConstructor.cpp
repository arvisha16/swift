//===--- SILGenConstructor.cpp - SILGen for constructors ------------------===//
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

#include "ArgumentSource.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyDelegates.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

static SILValue emitConstructorMetatypeArg(SILGenFunction &SGF,
                                           ValueDecl *ctor) {
  // In addition to the declared arguments, the constructor implicitly takes
  // the metatype as its first argument, like a static function.
  auto ctorFnType = ctor->getInterfaceType()->castTo<AnyFunctionType>();
  assert(ctorFnType->getParams().size() == 1 &&
         "more than one self parameter?");
  auto param = ctorFnType->getParams()[0];
  assert(!param.isVariadic() && !param.isInOut());
  Type metatype = param.getPlainType();
  auto *DC = ctor->getInnermostDeclContext();
  auto &AC = SGF.getASTContext();
  auto VD =
      new (AC) ParamDecl(VarDecl::Specifier::Default, SourceLoc(), SourceLoc(),
                         AC.getIdentifier("$metatype"), SourceLoc(),
                         AC.getIdentifier("$metatype"), DC);
  VD->setInterfaceType(metatype);

  SGF.AllocatorMetatype = SGF.F.begin()->createFunctionArgument(
      SGF.getLoweredType(DC->mapTypeIntoContext(metatype)), VD);

  return SGF.AllocatorMetatype;
}

// FIXME: Consolidate this with SILGenProlog
static RValue emitImplicitValueConstructorArg(SILGenFunction &SGF,
                                              SILLocation loc,
                                              CanType interfaceType,
                                              DeclContext *DC) {
  auto type = DC->mapTypeIntoContext(interfaceType)->getCanonicalType();

  // Restructure tuple arguments.
  if (auto tupleTy = dyn_cast<TupleType>(interfaceType)) {
    RValue tuple(type);
    for (auto fieldType : tupleTy.getElementTypes())
      tuple.addElement(emitImplicitValueConstructorArg(SGF, loc, fieldType, DC));
    return tuple;
  }

  auto &AC = SGF.getASTContext();
  auto VD = new (AC) ParamDecl(VarDecl::Specifier::Default, SourceLoc(), SourceLoc(),
                               AC.getIdentifier("$implicit_value"),
                               SourceLoc(),
                               AC.getIdentifier("$implicit_value"),
                               DC);
  VD->setInterfaceType(interfaceType);

  auto argType = SGF.SGM.Types.getLoweredType(type,
                                              ResilienceExpansion::Minimal);
  auto *arg = SGF.F.begin()->createFunctionArgument(argType, VD);
  ManagedValue mvArg;
  if (arg->getArgumentConvention().isOwnedConvention()) {
    mvArg = SGF.emitManagedRValueWithCleanup(arg);
  } else {
    mvArg = ManagedValue::forUnmanaged(arg);
  }

  // This can happen if the value is resilient in the calling convention
  // but not resilient locally.
  if (argType.isLoadable(SGF.SGM.M) && argType.isAddress()) {
    if (mvArg.isPlusOne(SGF))
      mvArg = SGF.B.createLoadTake(loc, mvArg);
    else
      mvArg = SGF.B.createLoadBorrow(loc, mvArg);
  }

  return RValue(SGF, loc, type, mvArg);
}

/// If the field has a property delegate for which we will need to call the
/// delegate type's init(initialValue:), set up that evaluation and call the
/// \c body with the expression to form the property delegate instance from
/// the initial value type.
///
/// \returns true if this was such a delegate, \c false otherwise.
static bool maybeEmitPropertyDelegateInitFromValue(
    SILGenFunction &SGF,
    SILLocation loc,
    VarDecl *field,
    RValue &&arg,
    llvm::function_ref<void(Expr *)> body) {
  auto originalProperty = field->getOriginalDelegatedProperty();
  if (!originalProperty ||
      !originalProperty->isPropertyDelegateInitializedWithInitialValue())
    return false;

  auto delegateInfo = originalProperty->getPropertyDelegateBackingPropertyInfo();
  if (!delegateInfo || !delegateInfo.initializeFromOriginal)
    return false;

  SILGenFunction::OpaqueValueRAII opaqueValue(
      SGF,
      delegateInfo.underlyingValue,
      std::move(arg).getAsSingleValue(SGF, loc));

  body(delegateInfo.initializeFromOriginal);
  return true;
}

static void emitImplicitValueConstructor(SILGenFunction &SGF,
                                         ConstructorDecl *ctor) {
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();
  // FIXME: Handle 'self' along with the other arguments.
  auto *paramList = ctor->getParameters();
  auto *selfDecl = ctor->getImplicitSelfDecl();
  auto selfIfaceTy = selfDecl->getInterfaceType();
  SILType selfTy = SGF.getLoweredType(selfDecl->getType());

  // Emit the indirect return argument, if any.
  SILValue resultSlot;
  if (selfTy.isAddressOnly(SGF.SGM.M) && SGF.silConv.useLoweredAddresses()) {
    auto &AC = SGF.getASTContext();
    auto VD = new (AC) ParamDecl(VarDecl::Specifier::InOut,
                                 SourceLoc(), SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 ctor);
    VD->setInterfaceType(selfIfaceTy);
    resultSlot = SGF.F.begin()->createFunctionArgument(selfTy.getAddressType(), VD);
  }

  // Emit the elementwise arguments.
  SmallVector<RValue, 4> elements;
  for (size_t i = 0, size = paramList->size(); i < size; ++i) {
    auto &param = paramList->get(i);

    elements.push_back(
      emitImplicitValueConstructorArg(
          SGF, Loc, param->getInterfaceType()->getCanonicalType(), ctor));
  }

  emitConstructorMetatypeArg(SGF, ctor);

  auto *decl = selfTy.getStructOrBoundGenericStruct();
  assert(decl && "not a struct?!");

  // If we have an indirect return slot, initialize it in-place.
  if (resultSlot) {
    auto elti = elements.begin(), eltEnd = elements.end();
    for (VarDecl *field : decl->getStoredProperties()) {
      auto fieldTy = selfTy.getFieldType(field, SGF.SGM.M);
      SILValue slot =
        SGF.B.createStructElementAddr(Loc, resultSlot, field,
                                      fieldTy.getAddressType());
      InitializationPtr init(new KnownAddressInitialization(slot));

      // If it's memberwise initialized, do so now.
      if (field->isMemberwiseInitialized(/*preferDeclaredProperties=*/false)) {
        assert(elti != eltEnd &&
               "number of args does not match number of fields");
        (void)eltEnd;
        FullExpr scope(SGF.Cleanups, field->getParentPatternBinding());
        if (!maybeEmitPropertyDelegateInitFromValue(
              SGF, Loc, field, std::move(*elti),
              [&](Expr *expr) {
                SGF.emitExprInto(expr, init.get());
              })) {
          std::move(*elti).forwardInto(SGF, Loc, init.get());
        }
        ++elti;
      } else {
#ifndef NDEBUG
        assert(
            field->getType()->isEqual(field->getParentInitializer()->getType())
              && "Checked by sema");
#endif

        // Cleanup after this initialization.
        FullExpr scope(SGF.Cleanups, field->getParentPatternBinding());
        SGF.emitExprInto(field->getParentInitializer(), init.get());
      }
    }
    SGF.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                       SGF.emitEmptyTuple(Loc));
    return;
  }

  // Otherwise, build a struct value directly from the elements.
  SmallVector<SILValue, 4> eltValues;

  auto elti = elements.begin(), eltEnd = elements.end();
  for (VarDecl *field : decl->getStoredProperties()) {
    auto fieldTy = selfTy.getFieldType(field, SGF.SGM.M);
    SILValue v;

    // If it's memberwise initialized, do so now.
    if (field->isMemberwiseInitialized(/*preferDeclaredProperties=*/false)) {
      FullExpr scope(SGF.Cleanups, field->getParentPatternBinding());
      assert(elti != eltEnd && "number of args does not match number of fields");
      (void)eltEnd;
      if (!maybeEmitPropertyDelegateInitFromValue(
            SGF, Loc, field, std::move(*elti),
            [&](Expr *expr) {
              v = SGF.emitRValue(expr)
                .forwardAsSingleStorageValue(SGF, fieldTy, Loc);
            })) {
        v = std::move(*elti).forwardAsSingleStorageValue(SGF, fieldTy, Loc);
      }
      ++elti;
    } else {
      // Otherwise, use its initializer.
      assert(field->isParentInitialized());

      // Cleanup after this initialization.
      FullExpr scope(SGF.Cleanups, field->getParentPatternBinding());
      v = SGF.emitRValue(field->getParentInitializer())
             .forwardAsSingleStorageValue(SGF, fieldTy, Loc);
    }

    eltValues.push_back(v);
  }

  SILValue selfValue = SGF.B.createStruct(Loc, selfTy, eltValues);
  SGF.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                     selfValue);
  return;
}

void SILGenFunction::emitValueConstructor(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  if (ctor->isMemberwiseInitializer())
    return emitImplicitValueConstructor(*this, ctor);

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
    ConstructorDecl::BodyInitKind::Delegating;

  // Get the 'self' decl and type.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto &lowering = getTypeLowering(selfDecl->getType());

  // Decide if we need to do extra work to warn on unsafe behavior in pre-Swift-5
  // modes.
  MarkUninitializedInst::Kind MUIKind;
  if (isDelegating) {
    MUIKind = MarkUninitializedInst::DelegatingSelf;
  } else if (getASTContext().isSwiftVersionAtLeast(5)) {
    MUIKind = MarkUninitializedInst::RootSelf;
  } else {
    auto *dc = ctor->getParent();
    if (isa<ExtensionDecl>(dc) &&
        dc->getSelfStructDecl()->getParentModule() != dc->getParentModule()) {
      MUIKind = MarkUninitializedInst::CrossModuleRootSelf;
    } else {
      MUIKind = MarkUninitializedInst::RootSelf;
    }
  }

  // Allocate the local variable for 'self'.
  emitLocalVariableWithCleanup(selfDecl, MUIKind)->finishInitialization(*this);
  SILValue selfLV = VarLocs[selfDecl].value;

  // Emit the prolog.
  emitProlog(ctor->getParameters(),
             /*selfParam=*/nullptr,
             ctor->getResultInterfaceType(), ctor,
             ctor->hasThrows());
  emitConstructorMetatypeArg(*this, ctor);

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit this until after we've emitted the body.
  // The epilog takes a void return because the return of 'self' is implicit.
  prepareEpilog(Type(), ctor->hasThrows(), CleanupLocation(ctor));

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto resultType = ctor->mapTypeIntoContext(ctor->getResultInterfaceType());
  auto &resultLowering = getTypeLowering(resultType);

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    // On failure, we'll clean up everything (except self, which should have
    // been cleaned up before jumping here) and return nil instead.
    SILGenSavedInsertionPoint savedIP(*this, failureBB,
                                      FunctionSection::Postmatter);
    failureExitBB = createBasicBlock();
    Cleanups.emitCleanupsForReturn(ctor, IsForUnwind);
    // Return nil.
    if (F.getConventions().hasIndirectSILResults()) {
      // Inject 'nil' into the indirect return.
      assert(F.getIndirectResults().size() == 1);
      B.createInjectEnumAddr(ctor, F.getIndirectResults()[0],
                             getASTContext().getOptionalNoneDecl());
      B.createBranch(ctor, failureExitBB);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, emitEmptyTuple(ctor));
    } else {
      // Pass 'nil' as the return value to the exit BB.
      failureExitArg = failureExitBB->createPhiArgument(
          resultLowering.getLoweredType(), ValueOwnershipKind::Owned);
      SILValue nilResult =
          B.createEnum(ctor, SILValue(), getASTContext().getOptionalNoneDecl(),
                       resultLowering.getLoweredType());
      B.createBranch(ctor, failureExitBB, nilResult);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, failureExitArg);
    }

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // If this is not a delegating constructor, emit member initializers.
  if (!isDelegating) {
    auto *typeDC = ctor->getDeclContext();
    auto *nominal = typeDC->getSelfNominalTypeDecl();
    emitMemberInitializers(ctor, selfDecl, nominal);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  
  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  SILValue selfValue;
  {
    SILGenSavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    assert(B.getInsertionBB()->empty() && "Epilog already set up?");
    
    auto cleanupLoc = CleanupLocation::get(ctor);

    if (!F.getConventions().hasIndirectSILResults()) {
      // Otherwise, load and return the final 'self' value.
      selfValue = lowering.emitLoad(B, cleanupLoc, selfLV,
                                    LoadOwnershipQualifier::Copy);

      // Inject the self value into an optional if the constructor is failable.
      if (ctor->getFailability() != OTK_None) {
        selfValue = B.createEnum(cleanupLoc, selfValue,
                                 getASTContext().getOptionalSomeDecl(),
                                 getLoweredLoadableType(resultType));
      }
    } else {
      // If 'self' is address-only, copy 'self' into the indirect return slot.
      assert(F.getConventions().getNumIndirectSILResults() == 1
             && "no indirect return for address-only ctor?!");

      // Get the address to which to store the result.
      SILValue completeReturnAddress = F.getIndirectResults()[0];
      SILValue returnAddress;
      switch (ctor->getFailability()) {
      // For non-failable initializers, store to the return address directly.
      case OTK_None:
        returnAddress = completeReturnAddress;
        break;
      // If this is a failable initializer, project out the payload.
      case OTK_Optional:
      case OTK_ImplicitlyUnwrappedOptional:
        returnAddress = B.createInitEnumDataAddr(cleanupLoc,
                                                 completeReturnAddress,
                                       getASTContext().getOptionalSomeDecl(),
                                                 selfLV->getType());
        break;
      }
      
      // We have to do a non-take copy because someone else may be using the
      // box (e.g. someone could have closed over it).
      B.createCopyAddr(cleanupLoc, selfLV, returnAddress,
                       IsNotTake, IsInitialization);
      
      // Inject the enum tag if the result is optional because of failability.
      if (ctor->getFailability() != OTK_None) {
        // Inject the 'Some' tag.
        B.createInjectEnumAddr(cleanupLoc, completeReturnAddress,
                               getASTContext().getOptionalSomeDecl());
      }
    }
  }
  
  // Finally, emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (!failureExitBB) {
      // If we're not returning self, then return () since we're returning Void.
      if (!selfValue) {
        SILLocation loc(ctor);
        loc.markAutoGenerated();
        selfValue = emitEmptyTuple(loc);
      }
      
      B.createReturn(returnLoc, selfValue);
    } else {
      if (selfValue)
        B.createBranch(returnLoc, failureExitBB, selfValue);
      else
        B.createBranch(returnLoc, failureExitBB);
    }
  }
}

void SILGenFunction::emitEnumConstructor(EnumElementDecl *element) {
  Type enumIfaceTy = element->getParentEnum()->getDeclaredInterfaceType();
  Type enumTy = F.mapTypeIntoContext(enumIfaceTy);
  auto &enumTI = SGM.Types.getTypeLowering(enumTy,
                                           ResilienceExpansion::Minimal);

  RegularLocation Loc(element);
  CleanupLocation CleanupLoc(element);
  Loc.markAutoGenerated();

  // Emit the indirect return slot.
  std::unique_ptr<Initialization> dest;
  if (enumTI.isAddressOnly() && silConv.useLoweredAddresses()) {
    auto &AC = getASTContext();
    auto VD = new (AC) ParamDecl(VarDecl::Specifier::InOut,
                                 SourceLoc(), SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 element->getDeclContext());
    VD->setInterfaceType(enumIfaceTy);
    auto resultSlot =
        F.begin()->createFunctionArgument(enumTI.getLoweredType(), VD);
    dest = std::unique_ptr<Initialization>(
        new KnownAddressInitialization(resultSlot));
  }

  Scope scope(Cleanups, CleanupLoc);

  // Emit the exploded constructor argument.
  ArgumentSource payload;
  if (element->hasAssociatedValues()) {
    auto eltArgTy = element->getArgumentInterfaceType()->getCanonicalType();
    RValue arg = emitImplicitValueConstructorArg(*this, Loc, eltArgTy, element);
    payload = ArgumentSource(Loc, std::move(arg));
  }

  // Emit the metatype argument.
  emitConstructorMetatypeArg(*this, element);

  // If possible, emit the enum directly into the indirect return.
  SGFContext C = (dest ? SGFContext(dest.get()) : SGFContext());
  ManagedValue mv = emitInjectEnum(Loc, std::move(payload),
                                   enumTI.getLoweredType(),
                                   element, C);

  // Return the enum.
  auto ReturnLoc = ImplicitReturnLocation::getImplicitReturnLoc(Loc);

  if (mv.isInContext()) {
    assert(enumTI.isAddressOnly());
    scope.pop();
    B.createReturn(ReturnLoc, emitEmptyTuple(Loc));
  } else {
    assert(enumTI.isLoadable() || !silConv.useLoweredAddresses());
    SILValue result = mv.ensurePlusOne(*this, ReturnLoc).forward(*this);
    scope.pop();
    B.createReturn(ReturnLoc, result);
  }
}

bool Lowering::usesObjCAllocator(ClassDecl *theClass) {
  // If the root class was implemented in Objective-C, use Objective-C's
  // allocation methods because they may have been overridden.
  return theClass->checkAncestry(AncestryFlags::ClangImported);
}

void SILGenFunction::emitClassConstructorAllocator(ConstructorDecl *ctor) {
  assert(!ctor->isFactoryInit() && "factories should not be emitted here");

  // Emit the prolog. Since we're just going to forward our args directly
  // to the initializer, don't allocate local variables for them.
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();

  // Forward the constructor arguments.
  // FIXME: Handle 'self' along with the other body patterns.
  SmallVector<SILValue, 8> args;
  bindParametersForForwarding(ctor->getParameters(), args);

  SILValue selfMetaValue = emitConstructorMetatypeArg(*this, ctor);

  // Allocate the "self" value.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  SILType selfTy = getLoweredType(selfDecl->getType());
  assert(selfTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");

  // Use alloc_ref to allocate the object.
  // TODO: allow custom allocation?
  // FIXME: should have a cleanup in case of exception
  auto selfClassDecl = ctor->getDeclContext()->getSelfClassDecl();

  SILValue selfValue;

  // Allocate the 'self' value.
  bool useObjCAllocation = usesObjCAllocator(selfClassDecl);

  if (ctor->hasClangNode() ||
      ctor->isObjCDynamic() ||
      ctor->isConvenienceInit()) {
    assert(ctor->hasClangNode() || ctor->isObjC());
    // For an allocator thunk synthesized for an @objc convenience initializer
    // or imported Objective-C init method, allocate using the metatype.
    SILValue allocArg = selfMetaValue;

    // When using Objective-C allocation, convert the metatype
    // argument to an Objective-C metatype.
    if (useObjCAllocation) {
      auto metaTy = allocArg->getType().castTo<MetatypeType>();
      metaTy = CanMetatypeType::get(metaTy.getInstanceType(),
                                    MetatypeRepresentation::ObjC);
      allocArg = B.createThickToObjCMetatype(Loc, allocArg,
                                             getLoweredType(metaTy));
    }

    selfValue = B.createAllocRefDynamic(Loc, allocArg, selfTy,
                                        useObjCAllocation, {}, {});
  } else {
    assert(ctor->isDesignatedInit());
    // For a designated initializer, we know that the static type being
    // allocated is the type of the class that defines the designated
    // initializer.
    selfValue = B.createAllocRef(Loc, selfTy, useObjCAllocation, false,
                                 ArrayRef<SILType>(), ArrayRef<SILValue>());
  }
  args.push_back(selfValue);

  // Call the initializer. Always use the Swift entry point, which will be a
  // bridging thunk if we're calling ObjC.
  auto initConstant = SILDeclRef(ctor, SILDeclRef::Kind::Initializer);

  ManagedValue initVal;
  SILType initTy;

  // Call the initializer.
  SubstitutionMap subMap;
  if (auto *genericEnv = ctor->getGenericEnvironmentOfContext()) {
    auto *genericSig = genericEnv->getGenericSignature();
    subMap = SubstitutionMap::get(
      genericSig,
      [&](SubstitutableType *t) -> Type {
        return genericEnv->mapTypeIntoContext(
          t->castTo<GenericTypeParamType>());
      },
      MakeAbstractConformanceForGenericType());
  }

  std::tie(initVal, initTy)
    = emitSiblingMethodRef(Loc, selfValue, initConstant, subMap);

  SILValue initedSelfValue = emitApplyWithRethrow(Loc, initVal.forward(*this),
                                                  initTy, subMap, args);

  emitProfilerIncrement(ctor->getBody());

  // Return the initialized 'self'.
  B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                 initedSelfValue);
}

void SILGenFunction::emitClassConstructorInitializer(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  assert(ctor->getBody() && "Class constructor without a body?");

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = false;
  if (!ctor->hasStubImplementation()) {
    isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
      ConstructorDecl::BodyInitKind::Delegating;
  }

  // Set up the 'self' argument.  If this class has a superclass, we set up
  // self as a box.  This allows "self reassignment" to happen in super init
  // method chains, which is important for interoperating with Objective-C
  // classes.  We also use a box for delegating constructors, since the
  // delegated-to initializer may also replace self.
  //
  // TODO: If we could require Objective-C classes to have an attribute to get
  // this behavior, we could avoid runtime overhead here.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto *dc = ctor->getDeclContext();
  auto selfClassDecl = dc->getSelfClassDecl();
  bool NeedsBoxForSelf = isDelegating ||
    (selfClassDecl->hasSuperclass() && !ctor->hasStubImplementation());
  bool usesObjCAllocator = Lowering::usesObjCAllocator(selfClassDecl);

  // If needed, mark 'self' as uninitialized so that DI knows to
  // enforce its DI properties on stored properties.
  MarkUninitializedInst::Kind MUKind;

  if (isDelegating) {
    if (ctor->isObjC())
      MUKind = MarkUninitializedInst::DelegatingSelfAllocated;
    else
      MUKind = MarkUninitializedInst::DelegatingSelf;
  } else if (selfClassDecl->requiresStoredPropertyInits() &&
             usesObjCAllocator) {
    // Stored properties will be initialized in a separate
    // .cxx_construct method called by the Objective-C runtime.
    assert(selfClassDecl->hasSuperclass() &&
           "Cannot use ObjC allocation without a superclass");
    MUKind = MarkUninitializedInst::DerivedSelfOnly;
  } else if (selfClassDecl->hasSuperclass())
    MUKind = MarkUninitializedInst::DerivedSelf;
  else
    MUKind = MarkUninitializedInst::RootSelf;

  if (NeedsBoxForSelf) {
    // Allocate the local variable for 'self'.
    emitLocalVariableWithCleanup(selfDecl, MUKind)->finishInitialization(*this);
  }

  // Emit the prolog for the non-self arguments.
  // FIXME: Handle self along with the other body patterns.
  uint16_t ArgNo = emitProlog(ctor->getParameters(), /*selfParam=*/nullptr,
                              TupleType::getEmpty(F.getASTContext()), ctor,
                              ctor->hasThrows());

  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  ManagedValue selfArg = B.createInputFunctionArgument(selfTy, selfDecl);

  if (!NeedsBoxForSelf) {
    SILLocation PrologueLoc(selfDecl);
    PrologueLoc.markAsPrologue();
    SILDebugVariable DbgVar(selfDecl->isLet(), ++ArgNo);
    B.createDebugValue(PrologueLoc, selfArg.getValue(), DbgVar);
  }

  if (!ctor->hasStubImplementation()) {
    assert(selfTy.hasReferenceSemantics() &&
           "can't emit a value type ctor here");
    if (NeedsBoxForSelf) {
      SILLocation prologueLoc = RegularLocation(ctor);
      prologueLoc.markAsPrologue();
      // SEMANTIC ARC TODO: When the verifier is complete, review this.
      B.emitStoreValueOperation(prologueLoc, selfArg.forward(*this),
                                VarLocs[selfDecl].value,
                                StoreOwnershipQualifier::Init);
    } else {
      selfArg = B.createMarkUninitialized(selfDecl, selfArg, MUKind);
      VarLocs[selfDecl] = VarLoc::get(selfArg.getValue());
    }
  }

  // Prepare the end of initializer location.
  SILLocation endOfInitLoc = RegularLocation(ctor);
  endOfInitLoc.pointToEnd();

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit the block until after we've emitted the body.
  prepareEpilog(Type(), ctor->hasThrows(),
                CleanupLocation::get(endOfInitLoc));

  auto resultType = ctor->mapTypeIntoContext(ctor->getResultInterfaceType());

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto &resultLowering = getTypeLowering(resultType);

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    RegularLocation loc(ctor);
    loc.markAutoGenerated();

    // On failure, we'll clean up everything and return nil instead.
    SILGenSavedInsertionPoint savedIP(*this, failureBB,
                                      FunctionSection::Postmatter);

    failureExitBB = createBasicBlock();
    failureExitArg = failureExitBB->createPhiArgument(
        resultLowering.getLoweredType(), ValueOwnershipKind::Owned);

    Cleanups.emitCleanupsForReturn(ctor, IsForUnwind);
    SILValue nilResult =
        B.createEnum(loc, SILValue(), getASTContext().getOptionalNoneDecl(),
                     resultLowering.getLoweredType());
    B.createBranch(loc, failureExitBB, nilResult);

    B.setInsertionPoint(failureExitBB);
    B.createReturn(loc, failureExitArg);

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // Handle member initializers.
  if (isDelegating) {
    // A delegating initializer does not initialize instance
    // variables.
  } else if (ctor->hasStubImplementation()) {
    // Nor does a stub implementation.
  } else if (selfClassDecl->requiresStoredPropertyInits() &&
             usesObjCAllocator) {
    // When the class requires all stored properties to have initial
    // values and we're using Objective-C's allocation, stored
    // properties are initialized via the .cxx_construct method, which
    // will be called by the runtime.

    // Note that 'self' has been fully initialized at this point.
  } else {
    // Emit the member initializers.
    emitMemberInitializers(ctor, selfDecl, selfClassDecl);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  // Emit the call to super.init() right before exiting from the initializer.
  if (NeedsBoxForSelf) {
    if (auto *SI = ctor->getSuperInitCall()) {
      B.setInsertionPoint(ReturnDest.getBlock());

      emitRValue(SI);

      B.emitBlock(B.splitBlockForFallthrough(), ctor);

      ReturnDest = JumpDest(B.getInsertionBB(),
                            ReturnDest.getDepth(),
                            ReturnDest.getCleanupLocation());
      B.clearInsertionPoint();
    }
  }

  CleanupStateRestorationScope SelfCleanupSave(Cleanups);

  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  {
    // Ensure that before we add additional cleanups, that we have emitted all
    // cleanups at this point.
    assert(!Cleanups.hasAnyActiveCleanups(getCleanupsDepth(),
                                          ReturnDest.getDepth()) &&
           "emitting epilog in wrong scope");

    SILGenSavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    auto cleanupLoc = CleanupLocation(ctor);

    // If we're using a box for self, reload the value at the end of the init
    // method.
    if (NeedsBoxForSelf) {
      ManagedValue storedSelf =
          ManagedValue::forUnmanaged(VarLocs[selfDecl].value);
      selfArg = B.createLoadCopy(cleanupLoc, storedSelf);
    } else {
      // We have to do a retain because we are returning the pointer +1.
      //
      // SEMANTIC ARC TODO: When the verifier is complete, we will need to
      // change this to selfArg = B.emitCopyValueOperation(...). Currently due
      // to the way that SILGen performs folding of copy_value, destroy_value,
      // the returned selfArg may be deleted causing us to have a
      // dead-pointer. Instead just use the old self value since we have a
      // class.
      selfArg = B.createCopyValue(cleanupLoc, selfArg);
    }

    // Inject the self value into an optional if the constructor is failable.
    if (ctor->getFailability() != OTK_None) {
      RegularLocation loc(ctor);
      loc.markAutoGenerated();
      selfArg = B.createEnum(loc, selfArg,
                             getASTContext().getOptionalSomeDecl(),
                             getLoweredLoadableType(resultType));
    }

    // Save our cleanup state. We want all other potential cleanups to fire, but
    // not this one.
    if (selfArg.hasCleanup())
      SelfCleanupSave.pushCleanupState(selfArg.getCleanup(),
                                       CleanupState::Dormant);

    // Translate our cleanup to the new top cleanup.
    //
    // This is needed to preserve the invariant in getEpilogBB that when
    // cleanups are emitted, everything above ReturnDest.getDepth() has been
    // emitted. This is not true if we use ManagedValue and friends in the
    // epilogBB, thus the translation. We perform the same check above that
    // getEpilogBB performs to ensure that we still do not have the same
    // problem.
    ReturnDest = std::move(ReturnDest).translate(getTopCleanup());
  }
  
  // Emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Unpop our selfArg cleanup, so we can forward.
  std::move(SelfCleanupSave).pop();

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (failureExitBB)
      B.createBranch(returnLoc, failureExitBB, selfArg.forward(*this));
    else
      B.createReturn(returnLoc, selfArg.forward(*this));
  }
}

static ManagedValue emitSelfForMemberInit(SILGenFunction &SGF, SILLocation loc,
                                          VarDecl *selfDecl) {
  CanType selfFormalType = selfDecl->getType()->getCanonicalType();
  if (selfFormalType->hasReferenceSemantics())
    return SGF.emitRValueForDecl(loc, selfDecl, selfFormalType,
                                 AccessSemantics::DirectToStorage,
                                 SGFContext::AllowImmediatePlusZero)
      .getAsSingleValue(SGF, loc);
  else
    return SGF.emitAddressOfLocalVarDecl(loc, selfDecl, selfFormalType,
                                         SGFAccessKind::Write);
}

static LValue emitLValueForMemberInit(SILGenFunction &SGF, SILLocation loc,
                                      VarDecl *selfDecl,
                                      VarDecl *property) {
  CanType selfFormalType = selfDecl->getType()->getCanonicalType();
  auto self = emitSelfForMemberInit(SGF, loc, selfDecl);
  return SGF.emitPropertyLValue(loc, self, selfFormalType, property,
                                LValueOptions(), SGFAccessKind::Write,
                                AccessSemantics::DirectToStorage);
}

/// Emit a member initialization for the members described in the
/// given pattern from the given source value.
static void emitMemberInit(SILGenFunction &SGF, VarDecl *selfDecl,
                           Pattern *pattern, RValue &&src) {
  switch (pattern->getKind()) {
  case PatternKind::Paren:
    return emitMemberInit(SGF, selfDecl,
                          cast<ParenPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Tuple: {
    auto tuple = cast<TuplePattern>(pattern);
    auto fields = tuple->getElements();

    SmallVector<RValue, 4> elements;
    std::move(src).extractElements(elements);
    for (unsigned i = 0, n = fields.size(); i != n; ++i) {
      emitMemberInit(SGF, selfDecl, fields[i].getPattern(),
                     std::move(elements[i]));
    }
    break;
  }

  case PatternKind::Named: {
    auto named = cast<NamedPattern>(pattern);
    // Form the lvalue referencing this member.
    FormalEvaluationScope scope(SGF);
    LValue memberRef = emitLValueForMemberInit(SGF, pattern, selfDecl,
                                               named->getDecl());

    // Assign to it.
    SGF.emitAssignToLValue(pattern, std::move(src), std::move(memberRef));
    return;
  }

  case PatternKind::Any:
    return;

  case PatternKind::Typed:
    return emitMemberInit(SGF, selfDecl,
                          cast<TypedPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Var:
    return emitMemberInit(SGF, selfDecl,
                          cast<VarPattern>(pattern)->getSubPattern(),
                          std::move(src));

#define PATTERN(Name, Parent)
#define REFUTABLE_PATTERN(Name, Parent) case PatternKind::Name:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("Refutable pattern in pattern binding");
  }
}

static Type getInitializationTypeInContext(
    DeclContext *fromDC, DeclContext *toDC,
    Pattern *pattern) {
  auto interfaceType = pattern->getType()->mapTypeOutOfContext();

  // If this pattern is initializing the backing storage for a property
  // with an attached delegate that is initialized with `=`, the
  // initialization type is the original property type.
  if (auto singleVar = pattern->getSingleVar()) {
    if (auto originalProperty = singleVar->getOriginalDelegatedProperty()) {
      if (originalProperty->isPropertyDelegateInitializedWithInitialValue())
        interfaceType = originalProperty->getValueInterfaceType();
    }
  }

  auto resultType = toDC->mapTypeIntoContext(interfaceType);

  return resultType;
}

void SILGenFunction::emitMemberInitializers(DeclContext *dc,
                                            VarDecl *selfDecl,
                                            NominalTypeDecl *nominal) {
  for (auto member : nominal->getMembers()) {
    // Find instance pattern binding declarations that have initializers.
    if (auto pbd = dyn_cast<PatternBindingDecl>(member)) {
      if (pbd->isStatic()) continue;

      for (auto entry : pbd->getPatternList()) {
        auto init = entry.getExecutableInit();
        if (!init) continue;

        // Cleanup after this initialization.
        FullExpr scope(Cleanups, entry.getPattern());

        // We want a substitution list written in terms of the generic
        // signature of the type, with replacement archetypes from the
        // constructor's context (which might be in an extension of
        // the type, which adds additional generic requirements).
        SubstitutionMap subs;
        auto *genericEnv = dc->getGenericEnvironmentOfContext();
        auto typeGenericSig = nominal->getGenericSignatureOfContext();

        if (genericEnv && typeGenericSig) {
          // Generate a set of substitutions for the initialization function,
          // whose generic signature is that of the type context, and whose
          // replacement types are the archetypes of the initializer itself.
          subs = SubstitutionMap::get(
            typeGenericSig,
            [&](SubstitutableType *type) {
              if (auto gp = type->getAs<GenericTypeParamType>()) {
                return genericEnv->mapTypeIntoContext(gp);
              }

              return Type(type);
            },
            LookUpConformanceInModule(dc->getParentModule()));
        }

        // Get the type of the initialization result, in terms
        // of the constructor context's archetypes.
        CanType resultType = getInitializationTypeInContext(
            pbd->getDeclContext(), dc, entry.getPattern())->getCanonicalType();
        AbstractionPattern origResultType(resultType);

        // FIXME: Can emitMemberInit() share code with
        // InitializationForPattern in SILGenDecl.cpp?
        RValue result = emitApplyOfStoredPropertyInitializer(
                                  init, entry, subs,
                                  resultType, origResultType,
                                  SGFContext());

        // If we have the backing storage for a property with an attached
        // property delegate initialized with `=`, inject the value into an
        // instance of the delegate.
        if (auto singleVar = pbd->getSingleVar()) {
          (void)maybeEmitPropertyDelegateInitFromValue(
              *this, init, singleVar, std::move(result),
              [&](Expr *expr) {
                result = emitRValue(expr);
              });
        }

        emitMemberInit(*this, selfDecl, entry.getPattern(), std::move(result));
      }
    }
  }
}

void SILGenFunction::emitIVarInitializer(SILDeclRef ivarInitializer) {
  auto cd = cast<ClassDecl>(ivarInitializer.getDecl());
  RegularLocation loc(cd);
  loc.markAutoGenerated();

  // Emit 'self', then mark it uninitialized.
  auto selfDecl = cd->getDestructor()->getImplicitSelfDecl();
  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  SILValue selfArg = F.begin()->createFunctionArgument(selfTy, selfDecl);
  SILLocation PrologueLoc(selfDecl);
  PrologueLoc.markAsPrologue();
  // Hard-code self as argument number 1.
  SILDebugVariable DbgVar(selfDecl->isLet(), 1);
  B.createDebugValue(PrologueLoc, selfArg, DbgVar);
  selfArg = B.createMarkUninitialized(selfDecl, selfArg,
                                      MarkUninitializedInst::RootSelf);
  assert(selfTy.hasReferenceSemantics() && "can't emit a value type ctor here");
  VarLocs[selfDecl] = VarLoc::get(selfArg);

  auto cleanupLoc = CleanupLocation::get(loc);
  prepareEpilog(TupleType::getEmpty(getASTContext()), false, cleanupLoc);

  // Emit the initializers.
  emitMemberInitializers(cd, selfDecl, cd);

  // Return 'self'.
  B.createReturn(loc, selfArg);

  emitEpilog(loc);
}

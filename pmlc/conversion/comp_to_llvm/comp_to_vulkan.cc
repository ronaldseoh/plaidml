// Copyright 2020, Intel Corporation
#include "pmlc/conversion/comp_to_llvm/passes.h"

#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SPIRV/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/Serialization.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"

#include "pmlc/conversion/comp_to_llvm/pass_detail.h"
#include "pmlc/conversion/comp_to_llvm/utils.h"
#include "pmlc/dialect/comp/ir/dialect.h"
#include "pmlc/util/logging.h"
#include "pmlc/util/tags.h"

namespace pmlc::conversion::comp_to_llvm {

namespace comp = pmlc::dialect::comp;
namespace gpu = mlir::gpu;
namespace LLVM = mlir::LLVM;
namespace spirv = mlir::spirv;

static constexpr const char *kVkInit = "vkInit";
static constexpr const char *kVkDeinit = "vkDeinit";
static constexpr const char *kVkRun = "vkRun";
static constexpr const char *kVkCreateLaunchKernelAction =
    "vkCreateLaunchKernelAction";
static constexpr const char *kVkSetLaunchKernelAction =
    "vkSetLaunchKernelAction";
static constexpr const char *kVkCreateMemoryTransferAction =
    "vkCreateMemoryTransferAction";
static constexpr const char *kVkWait = "vkWait";
static constexpr const char *kVkScheduleFunc = "vkScheduleFunc";

static constexpr const char *kVkBindBuffer = "vkBindBuffer";

namespace {

class ConvertCompToVulkanCall
    : public ConvertCompToVulkanCallBase<ConvertCompToVulkanCall> {
public:
  void runOnOperation();
};

void ConvertCompToVulkanCall::runOnOperation() {
  mlir::ModuleOp module = getOperation();

  uint32_t scheduleFuncNum = 0;
  module.walk([&](comp::ScheduleFunc op) { scheduleFuncNum++; });

  // Serialize SPIRV kernels.
  BinaryModulesMap modulesMap;
  if (mlir::failed(serializeSpirvKernels(module, modulesMap)))
    return signalPassFailure();

  // Populate conversion patterns.
  mlir::MLIRContext *context = &getContext();
  mlir::LLVMTypeConverter typeConverter{context};
  mlir::TypeConverter signatureConverter;
  mlir::OwningRewritePatternList patterns;
  populateCommonPatterns(context, typeConverter, signatureConverter, patterns);
  populateCompToVkPatterns(context, modulesMap, module, scheduleFuncNum,
                           typeConverter, patterns);
  // Set conversion target.
  mlir::ConversionTarget target(*context);
  target.addLegalDialect<LLVM::LLVMDialect>();
  target.addLegalDialect<mlir::StandardOpsDialect>();
  target.addIllegalDialect<comp::COMPDialect>();
  target.addDynamicallyLegalOp<mlir::FuncOp>([&](mlir::FuncOp op) -> bool {
    return typeConverter.isSignatureLegal(op.getType());
  });
  if (mlir::failed(mlir::applyPartialConversion(module, target, patterns)))
    signalPassFailure();
  // Insert runtime function declarations.
  addCommonFunctionDeclarations(module);
  addVkFunctionDeclarations(module, typeConverter);
}

template <class Op>
struct ConvertCompToVkBasePattern : ConvertCompOpBasePattern<Op> {
  ConvertCompToVkBasePattern(mlir::TypeConverter &typeConverter,
                             mlir::MLIRContext *context)
      : ConvertCompOpBasePattern<Op>(comp::ExecEnvRuntime::Vulkan,
                                     typeConverter, context) {}
};

/// Pattern for converting operation to llvm function call,
/// performing type conversions for results.
/// It can also handle variadic arguments when configured with
/// `varArg` and `nonVarArgs` constructor parameters.
template <class Op>
struct ConvertToFuncCallPattern : ConvertCompToVkBasePattern<Op> {
  ConvertToFuncCallPattern(mlir::StringRef funcName,
                           mlir::TypeConverter &typeConverter,
                           mlir::MLIRContext *context, bool varArg = false,
                           unsigned nonVarArgs = 0)
      : ConvertCompToVkBasePattern<Op>(typeConverter, context),
        funcName(funcName), varArg(varArg), nonVarArgs(nonVarArgs) {}

  mlir::LogicalResult
  matchAndRewrite(Op op, mlir::ArrayRef<mlir::Value> operands,
                  mlir::ConversionPatternRewriter &rewriter) const override;

  mlir::StringRef funcName;
  bool varArg;
  unsigned nonVarArgs;
};

using ConvertToInitVulkan = ConvertToFuncCallPattern<comp::CreateExecEnv>;
using ConvertToDeinitVulkan = ConvertToFuncCallPattern<comp::DestroyExecEnv>;
using ConvertWait = ConvertToFuncCallPattern<comp::Wait>;

struct ConvertScheduleFunc final
    : public mlir::ConvertOpToLLVMPattern<comp::ScheduleFunc> {
  ConvertScheduleFunc(const BinaryModulesMap &modulesMap,
                      mlir::ModuleOp &module, uint32_t scheduleFuncNum,
                      mlir::LLVMTypeConverter &typeConverter,
                      mlir::MLIRContext *context)
      : mlir::ConvertOpToLLVMPattern<comp::ScheduleFunc>(typeConverter),
        modulesMap(modulesMap), moduleOp(module),
        scheduleFuncNum(scheduleFuncNum) {
    pScheduleFuncIndex =
        reinterpret_cast<uint32_t *>(calloc(1, sizeof(uint32_t)));
    pBufferMap =
        new llvm::DenseMap<mlir::Value, llvm::SmallVector<uint64_t, 2>>();
  }

  mlir::LogicalResult
  matchAndRewrite(mlir::Operation *op, mlir::ArrayRef<mlir::Value> operands,
                  mlir::ConversionPatternRewriter &rewriter) const final;

  const BinaryModulesMap &modulesMap;
  uint32_t *pScheduleFuncIndex;
  mlir::ModuleOp moduleOp;
  uint32_t scheduleFuncNum;
  llvm::DenseMap<mlir::Value, llvm::SmallVector<uint64_t, 2>> *pBufferMap;
};

} // namespace

void populateCompToVkPatterns(mlir::MLIRContext *context,
                              const BinaryModulesMap &modulesMap,
                              mlir::ModuleOp module, uint32_t numKernel,
                              mlir::LLVMTypeConverter &typeConverter,
                              mlir::OwningRewritePatternList &patterns) {
  // Populate type conversion patterns.
  LLVM::LLVMType llvmInt8Ptr = LLVM::LLVMType::getInt8PtrTy(context);
  typeConverter.addConversion(
      [=](comp::ExecEnvType execEnvType) -> mlir::Optional<mlir::Type> {
        return llvmInt8Ptr;
      });
  typeConverter.addConversion(
      [=](comp::EventType eventType) -> mlir::Optional<mlir::Type> {
        return llvmInt8Ptr;
      });
  patterns.insert<ConvertToInitVulkan>(kVkInit, typeConverter, context);
  patterns.insert<ConvertToDeinitVulkan>(kVkDeinit, typeConverter, context);
  patterns.insert<ConvertScheduleFunc>(modulesMap, module, numKernel,
                                       typeConverter, context);
  patterns.insert<ConvertWait>(kVkWait, typeConverter, context,
                               /*varArg=*/true, /*nonVarArgs=*/0);
}

void addVkFunctionDeclarations(mlir::ModuleOp &module,
                               mlir::LLVMTypeConverter &typeConverter) {
  mlir::Location loc = module.getLoc();
  mlir::OpBuilder builder(module.getBody()->getTerminator());
  mlir::MLIRContext *context = builder.getContext();
  LLVM::LLVMType llvmInt8Ptr = LLVM::LLVMType::getInt8PtrTy(context);
  LLVM::LLVMType llvmVoid = LLVM::LLVMType::getVoidTy(context);
  LLVM::LLVMType llvmInt32 = LLVM::LLVMType::getInt32Ty(context);
  LLVM::LLVMType llvmInt64Type = LLVM::LLVMType::getInt64Ty(context);
  LLVM::LLVMType llvmIndex = typeConverter.getIndexType();

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkInit,
      LLVM::LLVMType::getFunctionTy(llvmInt8Ptr, {llvmInt8Ptr},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkCreateLaunchKernelAction,
      LLVM::LLVMType::getFunctionTy(llvmVoid,
                                    {llvmInt8Ptr, llvmInt8Ptr, llvmInt32,
                                     llvmInt8Ptr, llvmInt32, llvmInt32,
                                     llvmInt32},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkSetLaunchKernelAction,
      LLVM::LLVMType::getFunctionTy(llvmVoid, {llvmInt8Ptr, llvmInt32},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkRun,
      LLVM::LLVMType::getFunctionTy(llvmVoid, {llvmInt8Ptr},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkScheduleFunc,
      LLVM::LLVMType::getFunctionTy(llvmInt8Ptr, {llvmInt8Ptr},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkDeinit,
      LLVM::LLVMType::getFunctionTy(llvmVoid, {llvmInt8Ptr},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkWait,
      LLVM::LLVMType::getFunctionTy(llvmVoid, {llvmInt32},
                                    /*isVarArg=*/true));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkCreateMemoryTransferAction,
      LLVM::LLVMType::getFunctionTy(llvmVoid,
                                    {llvmInt8Ptr, llvmInt64Type, llvmInt64Type,
                                     llvmInt64Type, llvmInt64Type},
                                    /*isVarArg=*/false));

  builder.create<LLVM::LLVMFuncOp>(
      loc, kVkBindBuffer,
      LLVM::LLVMType::getFunctionTy(
          llvmVoid, {llvmInt8Ptr, llvmInt32, llvmInt32, llvmIndex, llvmInt8Ptr},
          /*isVarArg=*/false));
}

template <class Op>
mlir::LogicalResult ConvertToFuncCallPattern<Op>::matchAndRewrite(
    Op op, mlir::ArrayRef<mlir::Value> operands,
    mlir::ConversionPatternRewriter &rewriter) const {
  if (!this->isMatchingRuntime(op)) {
    return mlir::failure();
  }

  mlir::SmallVector<mlir::Type, 1> convertedTypes;
  for (mlir::Type prevType : op.getOperation()->getResultTypes()) {
    convertedTypes.push_back(this->convertType(prevType));
  }

  if (!varArg) {
    rewriter.replaceOpWithNewOp<LLVM::CallOp>(
        op.getOperation(), convertedTypes, rewriter.getSymbolRefAttr(funcName),
        operands);
    return mlir::success();
  }

  mlir::SmallVector<mlir::Value, 1> newOperands(operands.begin(),
                                                operands.begin() + nonVarArgs);
  LLVM::LLVMType llvmInt32Ty =
      LLVM::LLVMType::getInt32Ty(rewriter.getContext());
  mlir::Value varArgsCnt = rewriter.create<LLVM::ConstantOp>(
      op.getLoc(), llvmInt32Ty,
      rewriter.getI32IntegerAttr(operands.size() - nonVarArgs));
  newOperands.push_back(varArgsCnt);
  newOperands.insert(newOperands.end(), operands.begin() + nonVarArgs,
                     operands.end());

  rewriter.replaceOpWithNewOp<LLVM::CallOp>(op.getOperation(), convertedTypes,
                                            rewriter.getSymbolRefAttr(funcName),
                                            newOperands);
  return mlir::success();
}

mlir::LogicalResult ConvertScheduleFunc::matchAndRewrite(
    mlir::Operation *opPtr, mlir::ArrayRef<mlir::Value> operands,
    mlir::ConversionPatternRewriter &rewriter) const {
  auto op = mlir::cast<comp::ScheduleFunc>(opPtr);
  auto *parent = op.getParentOp();
  llvm::errs() << "\nLowering Vk ScheduleFunc:\n"
               << op << "\n\nin:\n\n"
               << *parent << "\n\n";

  mlir::Location loc = op.getLoc();
  auto launchOp = mlir::cast<gpu::LaunchFuncOp>(op.body().front().front());
  std::string binaryName = launchOp.getKernelModuleName().str();
  std::string kernelName = launchOp.getKernelName().str();

  // Create kernel from serialized binary.
  if (modulesMap.count(binaryName) == 0) {
    return mlir::failure();
  }
  if (modulesMap.at(binaryName).kernelsNameMap.count(kernelName) == 0) {
    return mlir::failure();
  }

  // containing all the args for kCreateVulkanLaunchKernelAction.
  std::vector<mlir::Value> createActionOperands{operands[0]};
  mlir::Value binaryPtr, binaryBytes;
  getPtrToBinaryModule(rewriter, loc, modulesMap.at(binaryName), binaryPtr,
                       binaryBytes);
  createActionOperands.push_back(binaryPtr);
  createActionOperands.push_back(binaryBytes);
  mlir::Value namePtr = getPtrToGlobalString(
      rewriter, loc, modulesMap.at(binaryName).kernelsNameMap.at(kernelName));
  createActionOperands.push_back(namePtr);

  LLVM::LLVMType llvmInt32Type =
      LLVM::LLVMType::getInt32Ty(rewriter.getContext());

  auto gSize = launchOp.getGridSizeOperandValues();
  auto x = gSize.x.getDefiningOp()->getAttrOfType<mlir::IntegerAttr>("value");
  auto y = gSize.y.getDefiningOp()->getAttrOfType<mlir::IntegerAttr>("value");
  auto z = gSize.z.getDefiningOp()->getAttrOfType<mlir::IntegerAttr>("value");
  mlir::Value gx = rewriter.create<LLVM::ConstantOp>(loc, llvmInt32Type, x);
  mlir::Value gy = rewriter.create<LLVM::ConstantOp>(loc, llvmInt32Type, y);
  mlir::Value gz = rewriter.create<LLVM::ConstantOp>(loc, llvmInt32Type, z);
  createActionOperands.push_back(gx);
  createActionOperands.push_back(gy);
  createActionOperands.push_back(gz);

  // transform mapped vulkan buffer to launch kernel.
  mlir::SmallVector<mlir::Value, 8> bufferOperands;
  mlir::SmallVector<mlir::Type, 8> bufferTypes;
  for (unsigned argI = 0; argI < launchOp.getNumKernelOperands(); ++argI) {
    auto operand = launchOp.getKernelOperand(argI);
    mlir::Value remappedArg = rewriter.getRemappedValue(operand);
    bufferOperands.push_back(remappedArg);
    bufferTypes.push_back(operand.getType());
  }

  LLVM::LLVMType llvmInt32Ty =
      LLVM::LLVMType::getInt32Ty(rewriter.getContext());
  LLVM::LLVMType llvmInt64Type =
      LLVM::LLVMType::getInt64Ty(rewriter.getContext());

  // Create LLVM constant for the descriptor set index.
  // Bind all memrefs to the `0` descriptor set, the same way as `GPUToSPIRV`
  // pass does.
  mlir::Value descriptorSet = rewriter.create<LLVM::ConstantOp>(
      loc, llvmInt32Ty, rewriter.getI32IntegerAttr(0));

  // Bind all buffers to Vulkan LaunchKernelAction.
  for (uint32_t bindIndex = 0; bindIndex < bufferOperands.size(); bindIndex++) {
    auto buffer = bufferOperands[bindIndex];
    // Create LLVM constant for the descriptor binding index.
    mlir::Value descriptorBinding = rewriter.create<LLVM::ConstantOp>(
        loc, llvmInt32Ty, rewriter.getI32IntegerAttr(bindIndex));

    auto memRefType =
        bufferTypes[bindIndex].dyn_cast_or_null<mlir::MemRefType>();
    if (!memRefType) {
      return mlir::failure();
    }

    // Figure out the amount of memory we need to allocate.
    mlir::SmallVector<mlir::Value, 4> sizes;
    getMemRefDescriptorSizes(loc, memRefType, {}, rewriter, sizes);
    auto bufferByteSize = getCumulativeSizeInBytes(
        loc, memRefType.getElementType(), sizes, rewriter);

    // Get a pointer to the underlying buffer.
    mlir::MemRefDescriptor bufferDesc{buffer};
    auto bufferPtr = bufferDesc.alignedPtr(rewriter, loc);
    auto bufferRaw =
        rewriter.create<LLVM::BitcastOp>(loc, getVoidPtrType(), bufferPtr);

    // Bind it.
    rewriter.create<LLVM::CallOp>(
        loc, getVoidType(), rewriter.getSymbolRefAttr(kVkBindBuffer),
        mlir::ArrayRef<mlir::Value>{operands[0], descriptorSet,
                                    descriptorBinding, bufferByteSize,
                                    bufferRaw});
  }

  rewriter.create<LLVM::CallOp>(
      loc, getVoidType(),
      rewriter.getSymbolRefAttr(kVkCreateLaunchKernelAction),
      createActionOperands);

  // Get subgroup size
  int64_t subgroupSize = 1;
  if (pmlc::hasIntegerTag(launchOp, "subgroupSize"))
    subgroupSize = pmlc::getIntegerTag(launchOp, "subgroupSize", 1);
  if (subgroupSize != 1) {
    IVLOG(2, "Subgroup size = " << subgroupSize);
  }

  mlir::Value subgroupSizeVal = rewriter.create<LLVM::ConstantOp>(
      loc, llvmInt32Ty, rewriter.getI32IntegerAttr(subgroupSize));

  rewriter.create<LLVM::CallOp>(
      loc, getVoidType(), rewriter.getSymbolRefAttr(kVkSetLaunchKernelAction),
      mlir::ArrayRef<mlir::Value>{operands[0], subgroupSizeVal});

  // Create Vulkan MemoryTransferAction
  auto &bufferMap = *pBufferMap;
  auto &scheduleFuncIndex = *pScheduleFuncIndex;
  for (size_t i = 0; i < bufferOperands.size(); i++) {
    for (auto pair : bufferMap) {
      if (pair.first == bufferOperands[i]) {
        mlir::Value dst_index = rewriter.create<LLVM::ConstantOp>(
            loc, llvmInt64Type,
            rewriter.getI64IntegerAttr(*pScheduleFuncIndex));
        mlir::Value dst_binding = rewriter.create<LLVM::ConstantOp>(
            loc, llvmInt64Type, rewriter.getI64IntegerAttr(i));
        mlir::Value src_index = rewriter.create<LLVM::ConstantOp>(
            loc, llvmInt64Type, rewriter.getI64IntegerAttr(pair.second[0]));
        mlir::Value src_binding = rewriter.create<LLVM::ConstantOp>(
            loc, llvmInt64Type, rewriter.getI64IntegerAttr(pair.second[1]));
        rewriter.create<LLVM::CallOp>(
            loc, getVoidType(),
            rewriter.getSymbolRefAttr(kVkCreateMemoryTransferAction),
            mlir::ArrayRef<mlir::Value>{operands[0], src_index, src_binding,
                                        dst_index, dst_binding});
      }
    }
    llvm::SmallVector<uint64_t, 2> second;
    second.append({scheduleFuncIndex, i});
    bufferMap[bufferOperands[i]] = second;
  }

  mlir::Type llvmEventType = this->typeConverter.convertType(op.getType());
  rewriter.replaceOpWithNewOp<LLVM::CallOp>(
      op.getOperation(), mlir::ArrayRef<mlir::Type>{llvmEventType},
      rewriter.getSymbolRefAttr(kVkScheduleFunc),
      mlir::ArrayRef<mlir::Value>{operands[0]});

  if (scheduleFuncIndex == scheduleFuncNum - 1) {
    rewriter.create<LLVM::CallOp>(loc, getVoidType(),
                                  rewriter.getSymbolRefAttr(kVkRun),
                                  mlir::ArrayRef<mlir::Value>{operands[0]});
  }

  scheduleFuncIndex++;
  llvm::errs() << "\nProduced:\n\n" << *parent << "\n\n";
  return mlir::success();
}

std::unique_ptr<mlir::Pass> createConvertCompToVulkanPass() {
  return std::make_unique<ConvertCompToVulkanCall>();
}

} // namespace pmlc::conversion::comp_to_llvm

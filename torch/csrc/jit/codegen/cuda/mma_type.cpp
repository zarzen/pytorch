#include <torch/csrc/jit/codegen/cuda/mma_type.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

MmaBuilder::MmaBuilder(
    MmaOptions::MacroType macro,
    MatMulTileOptions gemm_tile) {
  option_.macro = macro;
  // Calculate accumulator stride, will be removed once transpose swizzle ready
  int outer_stride = gemm_tile.warp_tile.n / gemm_tile.instruction_tile.n;
  switch (macro) {
    // Numbers depend on actual output layout of mma instruction
    case MmaOptions::MacroType::Volta_16_16_4:
      option_.accumulator_stride = outer_stride * 4;
      break;
    default:
      TORCH_CHECK(false, "unsupported macro");
      break;
  }
}

MmaBuilder& MmaBuilder::layout(MmaOptions::MmaInputLayout layout) {
  option_.operand_layout = layout;
  return *this;
}

MmaBuilder& MmaBuilder::operand(MmaOptions::Operand a_or_b) {
  option_.operand = a_or_b;
  return *this;
}

// TODO: validate op config
MmaOptions MmaBuilder::build() const {
  return option_;
}

bool isVolta(MmaOptions::MacroType macro) {
  return macro == MmaOptions::MacroType::Volta_16_16_4;
}

bool isTuring(MmaOptions::MacroType macro) {
  return macro == MmaOptions::MacroType::Turing_16_8_16;
}

bool isAmpere(MmaOptions::MacroType macro) {
  return false;
}

int getOutputRegisterSize(MmaOptions::MacroType macro) {
  switch (macro) {
    case MmaOptions::MacroType::Volta_16_16_4:
      return 8;
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "unknown macro");
      break;
  }
  return -1;
}

int getInputARegisterSize(MmaOptions::MacroType macro) {
  switch (macro) {
    case MmaOptions::MacroType::Volta_16_16_4:
      return 4;
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "unknown macro");
      break;
  }
  return -1;
}

int getInputBRegisterSize(MmaOptions::MacroType macro) {
  switch (macro) {
    case MmaOptions::MacroType::Volta_16_16_4:
      return 4;
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "unknown macro");
      break;
  }
  return -1;
}

bool isOperandTransposed(MmaOptions options) {
  switch (options.operand) {
    case MmaOptions::Operand::A:
      return options.operand_layout == MmaOptions::MmaInputLayout::TT ||
          options.operand_layout == MmaOptions::MmaInputLayout::TN;
    case MmaOptions::Operand::B:
      return options.operand_layout == MmaOptions::MmaInputLayout::TT ||
          options.operand_layout == MmaOptions::MmaInputLayout::NT;
    default:
      TORCH_CHECK(false, "isOperandTransposed: please specify operand");
  }
  return false;
}

std::string toString(MmaOptions::MmaInputLayout input_layout) {
  std::stringstream ss;
  switch (input_layout) {
    case MmaOptions::MmaInputLayout::TT:
      ss << "TT";
      break;
    case MmaOptions::MmaInputLayout::TN:
      ss << "TN";
      break;
    case MmaOptions::MmaInputLayout::NT:
      ss << "NT";
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "unsupported operand layout");
  }
  return ss.str();
}

std::string toString(MmaOptions::MacroType mt) {
  std::stringstream ss;
  switch (mt) {
    case MmaOptions::MacroType::NoMMA:
      ss << "NoOp";
      break;
    case MmaOptions::MacroType::Volta_16_16_4:
      ss << "M16N16K4";
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "undefined mma type");
      break;
  }
  return ss.str();
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch

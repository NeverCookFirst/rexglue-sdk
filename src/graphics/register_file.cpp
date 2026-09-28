/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/math.h>

namespace rex::graphics {

RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));

  // Hardware reset defaults of context registers a title may read before
  // writing (upstream xenia-canary e20f2696: read off a retail console and
  // matched against the R6xx reference and Mesa r600g). Tessellation levels
  // are left out on purpose: the backends apply them as register + 1.0f.
  values[XE_GPU_REG_VGT_MAX_VTX_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000;  // 8192 x 8192
  values[XE_GPU_REG_RB_STENCILREFMASK_BF] = 0x00FFFF00;
  values[XE_GPU_REG_PA_SU_POINT_SIZE] = 0x00080008;
  values[XE_GPU_REG_PA_SU_POINT_MINMAX] = 0x04000010;
  values[XE_GPU_REG_PA_SU_LINE_CNTL] = 0x00000008;
  values[XE_GPU_REG_PA_SC_LINE_CNTL] = 0x00000400;
  values[XE_GPU_REG_VGT_HOS_REUSE_DEPTH] = 0x0000000E;
  values[XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL] = 0x0000000E;
  values[XE_GPU_REG_VGT_OUT_DEALLOC_CNTL] = 0x00000010;
  values[XE_GPU_REG_PA_CL_GB_VERT_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_VERT_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_SC_AA_MASK] = 0x0000FFFF;
}

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_GPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterInfo reg_info = { \
        RegisterInfo::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace rex::graphics

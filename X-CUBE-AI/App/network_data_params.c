/**
  ******************************************************************************
  * @file    network_data_params.c
  * @author  AST Embedded Analytics Research Platform
  * @date    2026-09-18T15:53:13+0530
  * @brief   AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */

#include "network_data_params.h"


/**  Activations Section  ****************************************************/
ai_handle g_network_activations_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(NULL),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};




/**  Weights Section  ********************************************************/
AI_ALIGNED(32)
const ai_u64 s_network_weights_array_u64[24] = {
  0x22421a5540503fc3U, 0x17288c4a0657e981U, 0x74c87f1bfe083f4cU, 0xd7cbb60831cd18ceU,
  0xbe8163a7cb2bf6e3U, 0x31c8110b973afee3U, 0x7f12d1397b813610U, 0x95c3da910cc7f021U,
  0x2cd8477def221201U, 0xdd27f9e3a3de1d3U, 0xfbf405e0cb0581ceU, 0x8f744a6650b900d4U,
  0x4c8802087836377fU, 0xfffff906000004aeU, 0x800fffffe0bU, 0xc9e000008b2U,
  0xfffffc0f000000d5U, 0x19f67f7460dff421U, 0xd7eef8e6fb7f0beeU, 0x6fe1ca7fb4c2c5dbU,
  0x947fe1881638c5c7U, 0xffffff3c0000064fU, 0x869fffff21eU, 0xfffffffd7f0f3486U,
};


ai_handle g_network_weights_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(s_network_weights_array_u64),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};


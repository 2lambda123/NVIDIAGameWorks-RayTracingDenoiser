/*
Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "09_Resources.hlsl"
#include "09_RaytracingShared.hlsl"

[shader( "miss" )]
void Main_rmiss( inout Payload payload )
{
    // See InitPayload()
}

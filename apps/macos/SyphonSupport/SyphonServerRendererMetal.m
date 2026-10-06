/*
 SyphonServerRendererMetal.m
 Syphon
 
 Copyright 2020-2023 Maxime Touroute & Philippe Chaurand (www.millumin.com),
 bangnoise (Tom Butterworth) & vade (Anton Marini). All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 ReplayNES build of SyphonServerRendererMetal.m (replaces the upstream file of the same name in
 the Syphon framework target; see apps/macos/project.yml).

 The ONLY change from upstream (third_party/syphon, same file) is how the Metal library is
 obtained: upstream loads default.metallib, compiled offline from SyphonMetalShaders.metal by the
 Metal toolchain, which is an optional download in current Xcode. ReplayNES never needs the offline
 toolchain (see MetalView.swift), so the unmodified upstream shader source is embedded here at
 compile time (#embed) and compiled at runtime with -[MTLDevice newLibraryWithSource:].
 The rendering code below is identical to upstream.
 */

#import "SyphonServerRendererMetal.h"
#include <simd/simd.h>
#include "SyphonServerMetalTypes.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char kSyphonShaderSource[] = {
#embed "SyphonMetalShaders.metal"
    , 0
};
static const char kSyphonShaderTypes[] = {
#embed "SyphonServerMetalTypes.h"
    , 0
};
#pragma clang diagnostic pop

/// Upstream shader source with its local #include resolved (runtime compilation has no search path).
static NSString *SyphonRuntimeShaderSource(void)
{
    NSString *source = [NSString stringWithUTF8String:kSyphonShaderSource];
    NSString *types = [NSString stringWithUTF8String:kSyphonShaderTypes];
    return [source stringByReplacingOccurrencesOfString:@"#include \"SyphonServerMetalTypes.h\"" withString:types];
}

@implementation SyphonServerRendererMetal
{
    id<MTLRenderPipelineState> _pipelineState;
}

- (nonnull instancetype)initWithDevice:(id<MTLDevice>)device colorPixelFormat:(MTLPixelFormat)colorPixelFormat
{
    self = [super init];
    if( self )
    {
        NSError *error = NULL;
        id<MTLLibrary> defaultLibrary = [device newLibraryWithSource:SyphonRuntimeShaderSource() options:nil error:&error];
        if(!defaultLibrary)
        {
            NSLog(@"Syphon: Metal library could not be compiled: %@", error);
            return nil;
        }
        // Load the vertex/shader function from the library
        id <MTLFunction> vertexFunction = [defaultLibrary newFunctionWithName:@"textureToScreenVertexShader"];
        id <MTLFunction> fragmentFunction = [defaultLibrary newFunctionWithName:@"textureToScreenSamplingShader"];
        
        
        // Set up a descriptor for creating a pipeline state object
        MTLRenderPipelineDescriptor *pipelineStateDescriptor = [MTLRenderPipelineDescriptor new];
        pipelineStateDescriptor.label = @"Syphon Pipeline";
        pipelineStateDescriptor.vertexFunction = vertexFunction;
        pipelineStateDescriptor.fragmentFunction = fragmentFunction;
        pipelineStateDescriptor.colorAttachments[0].pixelFormat = colorPixelFormat;
        
        _pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
        
        
        if( !_pipelineState )
        {
            SYPHONLOG(@"Failed to createe pipeline state, error %@", error);
            return nil;
        }
    }
    return self;
}


- (void)renderFromTexture:(id<MTLTexture>)offScreenTexture inTexture:(id<MTLTexture>)texture region:(NSRect)region onCommandBuffer:(id<MTLCommandBuffer>)commandBuffer flip:(BOOL)flip
{
    if( texture == nil )
    {
        return;
    }
    
    const MTLViewport viewport = (MTLViewport){region.origin.x, region.origin.y, region.size.width, region.size.height, -1.0, 1.0 };
    vector_uint2 viewportSize = simd_make_uint2(viewport.width, viewport.height);
    
    const float w = viewport.width/2;
    const float h = viewport.height/2;
    const float flipValue = flip ? 1 : -1;
    
    const SYPHONTextureVertex quadVertices[] =
    {
        // Pixel positions (NDC), Texture coordinates
        { {  w,   flipValue * h },  { 1.f, 1.f } },
        { { -w,   flipValue * h },  { 0.f, 1.f } },
        { { -w,  flipValue * -h },  { 0.f, 0.f } },
        
        { {  w,  flipValue * h },  { 1.f, 1.f } },
        { { -w,  flipValue * -h },  { 0.f, 0.f } },
        { {  w,  flipValue * -h },  { 1.f, 0.f } },
    };
    
    const NSUInteger numberOfVertices = sizeof(quadVertices) / sizeof(SYPHONTextureVertex);
    MTLRenderPassDescriptor *renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    renderPassDescriptor.colorAttachments[0].texture = texture;
    renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    
    // Create a render command encoder so we can render into something
    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = @"Syphon Server Render Encoder";
    [renderEncoder setViewport:viewport];
    [renderEncoder setRenderPipelineState:_pipelineState];
    [renderEncoder setVertexBytes:quadVertices length:sizeof(quadVertices) atIndex:SYPHONVertexInputIndexVertices];
    [renderEncoder setVertexBytes:&viewportSize length:sizeof(viewportSize) atIndex:SYPHONVertexInputIndexViewportSize];
    [renderEncoder setFragmentTexture:offScreenTexture atIndex:SYPHONTextureIndexZero];
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:numberOfVertices];
    [renderEncoder endEncoding];
}

@end

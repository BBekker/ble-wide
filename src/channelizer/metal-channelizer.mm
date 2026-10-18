// SPDX-License-Identifier: MIT
#include "channelizer/metal-channelizer.h"
#include "metal-source.h"
#include "channelizer/channelizer.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <stdexcept>

static std::runtime_error failure(const char *what, NSError *error) {
  return std::runtime_error(
      std::string(what) + ": " +
      (error ? error.localizedDescription.UTF8String : "unavailable"));
}
struct MetalChannelizer::Impl {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> input, output, coefficients, oscillator, twiddle, roots5,
      ble_bins;
  double gpu_time = 0;
  Impl() {
    @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      if (!device)
        throw failure("Metal device", nil);
      queue = [device newCommandQueue];
      NSError *error = nil;
      MTLCompileOptions *options = [MTLCompileOptions new];
      // Match the CPU FIR accumulation closely, including weak signals.
      if (@available(macOS 15.0, *))
        options.mathMode = MTLMathModeSafe;
      else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO; // macOS before mathMode was available
#pragma clang diagnostic pop
      }
      id<MTLLibrary> library = [device newLibraryWithSource:@(metal_source)
                                                    options:options
                                                      error:&error];
      if (!library)
        throw failure("Metal shader compilation", error);
      pipeline = [device newComputePipelineStateWithFunction:
                             [library newFunctionWithName:@"channelize"]
                                                       error:&error];
      if (!pipeline || pipeline.maxTotalThreadsPerThreadgroup < 96)
        throw failure("Metal compute pipeline", error);
      if (!queue)
        throw failure("Metal command queue", nil);
      float h[1280], osc[320], tw[1280], roots[50];
      uint32_t bins[40];
      double taps[1280], total = 0;
      constexpr double pi = 3.14159265358979323846;
      for (int k = 0; k < 1280; ++k) {
        double x = k - 1279 / 2.0, a = 2 * pi * k / 1279;
        taps[k] =
            (x == 0 ? 2 * .65 / 160 : sin(2 * pi * .65 / 160 * x) / (pi * x)) *
            (.42 - .5 * cos(a) + .08 * cos(2 * a));
        total += taps[k];
      }
      for (int k = 0; k < 1280; ++k)
        h[k] = float(taps[k] / total);
      for (int k = 0; k < 160; ++k) {
        osc[2 * k] = float(cos(-2 * pi * k / 160));
        osc[2 * k + 1] = float(sin(-2 * pi * k / 160));
      }
      for (int p = 0; p < 16; ++p)
        for (int ch = 0; ch < 40; ++ch) {
          double a = 2 * pi * p * Channelizer::bin(ch) / 80;
          tw[2 * (p * 40 + ch)] = float(cos(a));
          tw[2 * (p * 40 + ch) + 1] = float(sin(a));
        }
      for (int ch = 0; ch < 40; ++ch)
        bins[ch] = Channelizer::bin(ch);
      for (int q = 0; q < 5; ++q)
        for (int s = 0; s < 5; ++s) {
          double a = 2 * pi * q * s / 5;
          roots[2 * (q * 5 + s)] = float(cos(a));
          roots[2 * (q * 5 + s) + 1] = float(sin(a));
        }
      roots5 = [device newBufferWithBytes:roots
                                   length:sizeof(roots)
                                  options:MTLResourceStorageModeShared];
      ble_bins = [device newBufferWithBytes:bins
                                     length:sizeof(bins)
                                    options:MTLResourceStorageModeShared];
      coefficients = [device newBufferWithBytes:h
                                         length:sizeof(h)
                                        options:MTLResourceStorageModeShared];
      oscillator = [device newBufferWithBytes:osc
                                       length:sizeof(osc)
                                      options:MTLResourceStorageModeShared];
      twiddle = [device newBufferWithBytes:tw
                                    length:sizeof(tw)
                                   options:MTLResourceStorageModeShared];
      if (!coefficients || !oscillator || !twiddle || !roots5 || !ble_bins)
        throw failure("Metal tables", nil);
    }
  }
};
MetalChannelizer::MetalChannelizer() : impl(std::make_unique<Impl>()) {}
MetalChannelizer::~MetalChannelizer() = default;
std::string MetalChannelizer::device_name() const {
  return impl->device.name.UTF8String;
}
double MetalChannelizer::gpu_seconds() const { return impl->gpu_time; }
void MetalChannelizer::run(const int16_t *iq, size_t samples,
                           std::vector<float> &bins, bool planar) {
  if (samples < 1280)
    throw std::runtime_error("Metal job is missing FIR history");
  size_t frames = (samples - 1280) / 80;
  bins.resize(frames * 80);
  if (!frames)
    return;
  @autoreleasepool {
    auto &m = *impl;
    size_t in_bytes = samples * 4, out_bytes = frames * 80 * sizeof(float);
    if (!m.input || m.input.length < in_bytes)
      m.input = [m.device newBufferWithLength:in_bytes
                                      options:MTLResourceStorageModeShared];
    if (!m.output || m.output.length < out_bytes)
      m.output = [m.device newBufferWithLength:out_bytes
                                       options:MTLResourceStorageModeShared];
    if (!m.input || !m.output)
      throw failure("Metal sample buffers", nil);
    memcpy(m.input.contents, iq, in_bytes);
    id<MTLCommandBuffer> command = [m.queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!command || !encoder)
      throw failure("Metal command", nil);
    [encoder setComputePipelineState:m.pipeline];
    [encoder setBuffer:m.input offset:0 atIndex:0];
    [encoder setBuffer:m.output offset:0 atIndex:1];
    [encoder setBuffer:m.coefficients offset:0 atIndex:2];
    [encoder setBuffer:m.oscillator offset:0 atIndex:3];
    [encoder setBuffer:m.twiddle offset:0 atIndex:4];
    [encoder setBuffer:m.roots5 offset:0 atIndex:5];
    [encoder setBuffer:m.ble_bins offset:0 atIndex:6];
    uint32_t layout[2] = {uint32_t(frames), uint32_t(planar)};
    [encoder setBytes:layout length:sizeof(layout) atIndex:7];
    [encoder dispatchThreadgroups:MTLSizeMake(frames, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(96, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
      throw failure("Metal execution", command.error);
    m.gpu_time += command.GPUEndTime - command.GPUStartTime;
    memcpy(bins.data(), m.output.contents, out_bytes);
  }
}

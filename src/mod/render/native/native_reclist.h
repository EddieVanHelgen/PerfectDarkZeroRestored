// Native renderer, parallel recording (3 Nov 2026, docs/PERF_FOR_EVERYONE.md "Part B"): the frame command list as the
// draw path sees it (NativeDevice::list()).
//
// pdz_native_record_workers = 1 (default): every method calls the D3D12 command list at once, exactly as before.
// pdz_native_record_workers = 2: every method copies its arguments into a byte stream instead, and a second thread
// (the replay thread) makes the same D3D12 calls on the same command list, in the same order, with the same
// arguments. The record thread keeps all of the renderer's own work (state decode, caches, uploads, descriptors);
// only the D3D12 recording calls and the driver time inside them move to the replay thread. The list is closed,
// executed and signalled on the record thread as before, after a drain (the replay thread has made every call
// recorded so far), so submission order, fences and everything the guest sees are unchanged.
//
// What the deferral changes and how it is covered (the design notes in docs/PERF_FOR_EVERYONE.md):
//   - D3D12 reads RTV / DSV descriptors when OMSetRenderTargets / Clear*View is recorded: NativeDevice::FreeRtv /
//     FreeDsv hold freed slots until the next drain, so no slot a pending call names is reused or rewritten.
//   - Everything else a call names is read by the GPU at execution (after the drain): shader-visible descriptors,
//     upload memory, resources (released through ReleaseLater, after the frame's fence).
//   - Arrays passed by pointer (barriers, viewports, scissors, render targets, root constants, clear rects, copy
//     locations) are copied into the stream.
//   - Close / Reset / ExecuteCommandLists stay with NativeDevice, after Drain().
// Producers: whoever holds RenderMutex() (the record thread; inline payloads on the game thread); one at a time.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <d3d12.h>

namespace pdz::native
{
  // The payloads of the fixed-size records (the replay thread reads the same layouts).
  namespace reclist
  {
    struct Draw { UINT a, b, c, d; };
    struct DrawIndexed { UINT a, b, c; INT d; UINT e; };
    struct Dispatch3 { UINT x, y, z; };
    struct Copy { ID3D12Resource *dst, *src; };
    struct CopyRegion
    {
      D3D12_TEXTURE_COPY_LOCATION dst, src;
      D3D12_BOX box;
      UINT x, y, z, has_box;
    };
    struct BlendFactor { FLOAT f[4]; UINT null; };
    struct Table { UINT64 h; UINT index, pad; };
    struct Cbv { UINT64 address; UINT index, pad; };
    struct IndexBuffer { D3D12_INDEX_BUFFER_VIEW v; UINT null, pad; };
    struct Query { ID3D12QueryHeap *heap; D3D12_QUERY_TYPE type; UINT index; };
    struct Resolve
    {
      ID3D12QueryHeap *heap;
      ID3D12Resource *dst;
      UINT64 offset;
      D3D12_QUERY_TYPE type;
      UINT start, count, pad;
    };
    // Variable-size records: a fixed head, then the items.
    struct ItemsHead { UINT count, pad; };
    struct ConstHead { UINT index, count, offset, pad; };
    struct VbHead { UINT start, count, null, pad; };
    struct RtHead { UINT count, single_range, has_ds, pad; D3D12_CPU_DESCRIPTOR_HANDLE ds; };
    struct ClearDepthHead { D3D12_CPU_DESCRIPTOR_HANDLE view; D3D12_CLEAR_FLAGS flags; FLOAT depth; UINT stencil; UINT rects; };
    struct ClearColorHead { D3D12_CPU_DESCRIPTOR_HANDLE view; FLOAT rgba[4]; UINT rects, pad; };
  } // namespace reclist

  class RecList
  {
  public:
    enum class Op : uint16_t
    {
      kWrap, // the rest of the ring is unused: continue at offset 0
      kDrawInstanced,
      kDrawIndexedInstanced,
      kDispatch,
      kCopyResource,
      kCopyTextureRegion,
      kIASetPrimitiveTopology,
      kRSSetViewports,
      kRSSetScissorRects,
      kOMSetBlendFactor,
      kOMSetStencilRef,
      kSetPipelineState,
      kResourceBarrier,
      kSetDescriptorHeaps,
      kSetComputeRootSignature,
      kSetGraphicsRootSignature,
      kSetComputeRootDescriptorTable,
      kSetGraphicsRootDescriptorTable,
      kSetComputeRoot32BitConstants,
      kSetGraphicsRoot32BitConstants,
      kSetGraphicsRootConstantBufferView,
      kIASetIndexBuffer,
      kIASetVertexBuffers,
      kOMSetRenderTargets,
      kClearDepthStencilView,
      kClearRenderTargetView,
      kBeginQuery,
      kEndQuery,
      kResolveQueryData,
      kCount
    };

    // NativeDevice: the real list (created at device init).
    void Bind(ID3D12GraphicsCommandList *real) { real_ = real; }
    ID3D12GraphicsCommandList *real() const { return real_; }
    bool deferred() const { return deferred_; }
    // NativeDevice: the real list was reset (open: calls are deferred) / is about to be closed (after Drain). While it
    // is closed, calls go to the real list at once, as before (the replay thread is idle then: drained at the close),
    // so a call made between frames reaches D3D12 exactly as it did without the replay thread.
    void SetOpen(bool open)
    {
      open_ = open;
      defer_ = deferred_ && open && !direct_test_;
    }
    // Calls made on the closed list (diagnostic; counted with the replay thread on).
    uint64_t closed_calls() const { return closed_calls_; }

    // NativeDevice::BeginFrame, with the list closed: starts the replay thread when pdz_native_record_workers is 2,
    // or 0 (auto) and enough cores (decided once, at frame 600 for auto, else at the first call).
    void StartIfWanted(uint64_t frame);
    // Publishes what was recorded so far to the replay thread (cheap; the stream also publishes itself every few
    // calls). The record thread calls it when it runs out of commands, so the replay thread never waits on a
    // stream the record thread is not adding to.
    void Publish();
    // Waits until the replay thread has made every recorded call (the list can then be closed / executed). Returns
    // at once when not deferred.
    void Drain();
    // Shutdown: drains and stops the replay thread (calls go direct again).
    void Stop();

    // ----- ID3D12GraphicsCommandList, the methods the native renderer uses ---------------------------------------
    void DrawInstanced(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
        return;
      }
      reclist::Draw args{vertex_count, instance_count, start_vertex, start_instance};
      Put(Op::kDrawInstanced, &args, sizeof(args));
      ++draws_recorded_;
    }
    void DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT start_index, INT base_vertex,
                              UINT start_instance)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->DrawIndexedInstanced(index_count, instance_count, start_index, base_vertex, start_instance);
        return;
      }
      reclist::DrawIndexed args{index_count, instance_count, start_index, base_vertex,
                                                      start_instance};
      Put(Op::kDrawIndexedInstanced, &args, sizeof(args));
      ++draws_recorded_;
    }
    void Dispatch(UINT x, UINT y, UINT z)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->Dispatch(x, y, z);
        return;
      }
      reclist::Dispatch3 args{x, y, z};
      Put(Op::kDispatch, &args, sizeof(args));
    }
    void CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->CopyResource(dst, src);
        return;
      }
      reclist::Copy args{dst, src};
      Put(Op::kCopyResource, &args, sizeof(args));
    }
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->CopyTextureRegion(dst, x, y, z, src, box);
        return;
      }
      reclist::CopyRegion args{*dst, *src, box ? *box : D3D12_BOX{}, x, y, z, box ? 1u : 0u};
      Put(Op::kCopyTextureRegion, &args, sizeof(args));
    }
    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->IASetPrimitiveTopology(t);
        return;
      }
      Put(Op::kIASetPrimitiveTopology, &t, sizeof(t));
    }
    void RSSetViewports(UINT count, const D3D12_VIEWPORT *v)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->RSSetViewports(count, v);
        return;
      }
      PutArray(Op::kRSSetViewports, count, v, sizeof(D3D12_VIEWPORT));
    }
    void RSSetScissorRects(UINT count, const D3D12_RECT *r)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->RSSetScissorRects(count, r);
        return;
      }
      PutArray(Op::kRSSetScissorRects, count, r, sizeof(D3D12_RECT));
    }
    void OMSetBlendFactor(const FLOAT factor[4])
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->OMSetBlendFactor(factor);
        return;
      }
      reclist::BlendFactor args{};
      if (factor)
      {
        std::memcpy(args.f, factor, sizeof(args.f));
      }
      args.null = factor ? 0u : 1u;
      Put(Op::kOMSetBlendFactor, &args, sizeof(args));
    }
    void OMSetStencilRef(UINT ref)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->OMSetStencilRef(ref);
        return;
      }
      Put(Op::kOMSetStencilRef, &ref, sizeof(ref));
    }
    void SetPipelineState(ID3D12PipelineState *p)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetPipelineState(p);
        return;
      }
      Put(Op::kSetPipelineState, &p, sizeof(p));
    }
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *b)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->ResourceBarrier(count, b);
        return;
      }
      PutArray(Op::kResourceBarrier, count, b, sizeof(D3D12_RESOURCE_BARRIER));
    }
    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap *const *heaps)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetDescriptorHeaps(count, heaps);
        return;
      }
      PutArray(Op::kSetDescriptorHeaps, count, heaps, sizeof(ID3D12DescriptorHeap *));
    }
    void SetComputeRootSignature(ID3D12RootSignature *rs)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetComputeRootSignature(rs);
        return;
      }
      Put(Op::kSetComputeRootSignature, &rs, sizeof(rs));
    }
    void SetGraphicsRootSignature(ID3D12RootSignature *rs)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetGraphicsRootSignature(rs);
        return;
      }
      Put(Op::kSetGraphicsRootSignature, &rs, sizeof(rs));
    }
    void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetComputeRootDescriptorTable(index, h);
        return;
      }
      reclist::Table args{h.ptr, index, 0};
      Put(Op::kSetComputeRootDescriptorTable, &args, sizeof(args));
    }
    void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetGraphicsRootDescriptorTable(index, h);
        return;
      }
      reclist::Table args{h.ptr, index, 0};
      Put(Op::kSetGraphicsRootDescriptorTable, &args, sizeof(args));
    }
    void SetComputeRoot32BitConstants(UINT index, UINT count, const void *data, UINT offset)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetComputeRoot32BitConstants(index, count, data, offset);
        return;
      }
      PutConstants(Op::kSetComputeRoot32BitConstants, index, count, data, offset);
    }
    void SetGraphicsRoot32BitConstants(UINT index, UINT count, const void *data, UINT offset)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetGraphicsRoot32BitConstants(index, count, data, offset);
        return;
      }
      PutConstants(Op::kSetGraphicsRoot32BitConstants, index, count, data, offset);
    }
    void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->SetGraphicsRootConstantBufferView(index, address);
        return;
      }
      reclist::Cbv args{address, index, 0};
      Put(Op::kSetGraphicsRootConstantBufferView, &args, sizeof(args));
    }
    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->IASetIndexBuffer(view);
        return;
      }
      reclist::IndexBuffer args{view ? *view : D3D12_INDEX_BUFFER_VIEW{},
                                                                   view ? 0u : 1u, 0};
      Put(Op::kIASetIndexBuffer, &args, sizeof(args));
    }
    void IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->IASetVertexBuffers(start, count, views);
        return;
      }
      PutVertexBuffers(start, count, views);
    }
    void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rts, BOOL single_range,
                            const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->OMSetRenderTargets(count, rts, single_range, ds);
        return;
      }
      PutRenderTargets(count, rts, single_range, ds);
    }
    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                               UINT rect_count, const D3D12_RECT *rects)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->ClearDepthStencilView(view, flags, depth, stencil, rect_count, rects);
        return;
      }
      PutClearDepth(view, flags, depth, stencil, rect_count, rects);
    }
    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT rgba[4], UINT rect_count,
                               const D3D12_RECT *rects)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->ClearRenderTargetView(view, rgba, rect_count, rects);
        return;
      }
      PutClearColor(view, rgba, rect_count, rects);
    }
    void BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->BeginQuery(heap, type, index);
        return;
      }
      reclist::Query args{heap, type, index};
      Put(Op::kBeginQuery, &args, sizeof(args));
    }
    void EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->EndQuery(heap, type, index);
        return;
      }
      reclist::Query args{heap, type, index};
      Put(Op::kEndQuery, &args, sizeof(args));
    }
    void ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst,
                          UINT64 offset)
    {
      if (!defer_)
      {
        closed_calls_ += deferred_; // the replay thread on, the list closed
        real_->ResolveQueryData(heap, type, start, count, dst, offset);
        return;
      }
      reclist::Resolve args{heap, dst, offset, type, start, count, 0};
      Put(Op::kResolveQueryData, &args, sizeof(args));
    }

    // Draw calls recorded (deferred mode), for the replay thread's per-frame check.
    uint64_t draws_recorded() const { return draws_recorded_; }

  private:
    // Appends one record: a 16-byte header (op, payload bytes, sequence) and the payload padded to 16.
    void Put(Op op, const void *data, uint32_t bytes)
    {
      uint8_t *p = Begin(op, bytes);
      std::memcpy(p, data, bytes);
      End();
    }
    void PutArray(Op op, UINT count, const void *items, uint32_t item_bytes);
    void PutConstants(Op op, UINT index, UINT count, const void *data, UINT offset);
    void PutVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views);
    void PutRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rts, BOOL single_range,
                          const D3D12_CPU_DESCRIPTOR_HANDLE *ds);
    void PutClearDepth(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                       UINT rect_count, const D3D12_RECT *rects);
    void PutClearColor(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT rgba[4], UINT rect_count,
                       const D3D12_RECT *rects);
    // Reserves a record of `bytes` payload bytes; returns the payload pointer. End() commits it.
    uint8_t *Begin(Op op, uint32_t bytes);
    void End();

    ID3D12GraphicsCommandList *real_ = nullptr;
    bool deferred_ = false;
    bool open_ = false;
    bool defer_ = false; // deferred_ && open_
    bool direct_test_ = false; // pdz_native_record_workers_direct (test aid)
    uint64_t closed_calls_ = 0;
    bool start_checked_ = false;
    uint64_t draws_recorded_ = 0;
    uint32_t unpublished_ = 0;
    uint64_t pending_end_ = 0; // Begin's record end (End publishes up to it)
  };
} // namespace pdz::native

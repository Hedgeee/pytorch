#pragma once

#include <ATen/native/vulkan/ops/Common.h>
#include <ATen/native/vulkan/VulkanOpaqueTensorImpl.h>

namespace at {
namespace native {
namespace vulkan {
namespace ops {

//
// This class represents a Vulkan tensor and provides an abstraction layer
// that allows both the CPU, and the GPU, to view a Vulkan (buffer, image)
// pair as one coherent, synchronized unit of storage on both UMA and NUMA
// systems.  Expanding on the previous sentence, this class tries to address
// two orthogonal implementation complexities that arise as a result of the
// aforementioned goal of memory coherence:
//
// 1) First, synchronization across processors; CPUs and GPUs are separate
//    processors, and even though they share the same address space in a system
//    with a unified memory architecture, their address spaces only partially
//    overlap on NUMA.  Consequently on NUMA, while it is still technically
//    possible to take advantage of this shared address space to maintain one
//    single copy of the data, different access latencies from CPU and GPU to
//    this shared location usually necessitates maintaining two copies each in
//    processor-local memory, otherwise memory access latency will hurt from
//    the processor to which this data is not close.  This shared memory is more
//    often than not located in system memory, making for slow GPU read and
//    write access over the PCI-e bus.  Maintaining two separate copies on the
//    other hand, requires synchronization to guarantee coherence.  This is
//    not an issue on UMA and this implementation accounts for that optimization.
//
// 2) Second, synchronization across resources (i.e. buffers and images); GPU
//    drivers pack images in proprietory formats for better locality of access
//    and to enable lossless compression.  These conversions are both expensive
//    (in general) and manual (in Vulkan.)  This requires a second order of
//    synchronization to guarantee coherence between the contents of the buffer
//    and image otherwise they will go out of sync.
//
// It is extremely important to keep in mind that the functionality this class
// provides is generally expensive.  For optimal performance, the user of this
// class should:
//
// 1) Avoid frequent CPU <=> GPU transfers which will be triggered if data is
//    write accessed on one processor and read / write accessed on the other.
//
// 2) Avoid frequent buffer <=> image conversions which will be trigerred if
//    data is write accessed as a buffer (image) and read accessed as an
//    image (buffer).
//
// For optimal performance, access the data as images, and keep the data on GPU,
// and above all understand the expensive data flow that this class abstracts
// away.
//
// vTensor tries to address a specific concern and intentionally does not expose
// GPU tensor memory directly.  Please keep that behavior intact as the whole
// data model fundamentally depends on limiting what the user can achieve through
// the interface to guarantee performance and coherence.
//
// A vTensor is associated with an api::Context as preparation for multi-GPU
// support.
//

class C10_EXPORT vTensor final {
 public:
  vTensor();
  vTensor(
      api::Context* context,
      IntArrayRef sizes,
      const TensorOptions& options);

  /*
    Access
  */

  typedef api::Resource::Memory::Access Access;

  /*
    Future
  */

  template<typename Type, Access::Flags kAccess>
  class Future final {
    template<typename T, Access::Flags A>
    using is_convertible = std::enable_if_t<
        std::is_convertible<
            Access::Pointer<T, A>,
            Access::Pointer<Type, kAccess>>::value>;

   public:
    explicit Future(vTensor* tensor);
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&);
    Future& operator=(Future&&) &;
    Future& operator=(Future&&) && = delete;
    template<typename T, Access::Flags A, typename = is_convertible<T, A>>
    Future(Future<T, A>&&);
    template<typename T, Access::Flags A, typename = is_convertible<T, A>>
    Future& operator=(Future<T, A>&&) &;
    template<typename T, Access::Flags A>
    Future& operator=(Future<T, A>&&) && = delete;
    ~Future();

    typedef api::Resource::Memory::Data<
        Access::Pointer<
            Type,
            kAccess>> Payload;

    Payload wait() const &;

   private:
    // Intentionally disabed to enforce a usage pattern wherein the Future's
    // lifetime exceeds that of the Payload as we use the Future's destructor
    // to eagerly (as opposed to lazily and upon first use) upload the
    // modifications back onto GPU in an effort to hide the copy latency.

    Payload wait() const && = delete;

   private:
    template<typename, Access::Flags>
    friend class Future;

   private:
    vTensor* tensor_;
  };

  /*
    Host access - these functions can be expensive.
  */

  template<typename Type>
  Future<Type, Access::Read> host() const &;

  template<typename Type, Access::Flags kAccess>
  Future<Type, kAccess> host() &;

  /*
    Device access - these functions can be expensive.
  */

  VkBuffer buffer() const &;
  VkBuffer buffer(Access::Flags access) &;

  VkImage image() const &;
  VkImage image(Access::Flags access) &;

 private:
  const vTensor* host() const;
  vTensor* host(Access::Flags access);

  // These overloads are intentionally disabled to enforce a usage pattern
  // wherein the Tensor's lifetime exceeds that of the scope in which the
  // underlying data is accessed.  Allowing below overloads to be invoked
  // on a temporary would open the door to the possibility of accessing the
  // underlying memory out of the expected scope.

  template<typename Type>
  Future<Type, Access::Read> host() const && = delete;

  template<typename Type, Access::Flags kAccess>
  Future<Type, kAccess> host() && = delete;

  VkBuffer buffer() const && = delete;
  VkBuffer buffer(Access::Flags access) && = delete;

  VkImage image() const && = delete;
  VkImage image(Access::Flags access) && = delete;

 private:
  api::Resource::Image image_;
  api::Resource::Buffer buffer_;
  api::Resource::Buffer staging_;
  api::Context* context_;
  c10::SmallVector<int64_t, 4u> sizes_;
  TensorOptions options_;

  mutable struct {
    uint32_t image : 1u;
    uint32_t buffer : 1u;
    uint32_t staging : 1u;
  } dirty_;
};

using vTensorImpl = VulkanOpaqueTensorImpl<vTensor>;
void verify(const TensorOptions& options);

//
// Impl
//

template<typename Type, vTensor::Access::Flags kAccess>
inline vTensor::Future<Type, kAccess>::Future(
    vTensor* const tensor)
  : tensor_(tensor) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      tensor_,
      "Invalid Vulkan tensor!");
}

template<typename Type, vTensor::Access::Flags kAccess>
inline vTensor::Future<Type, kAccess>::Future(
    Future&& future)
  : tensor_(std::move(future.tensor_)) {
  future.tensor_ = nullptr;
}

template<typename Type, vTensor::Access::Flags kAccess>
inline vTensor::Future<Type, kAccess>&
vTensor::Future<Type, kAccess>::operator=(
    Future&& future) & {
  tensor_ = std::move(future.tensor_);
  future.tensor_ = nullptr;
  return *this;
}

template<typename Type, vTensor::Access::Flags kAccess>
template<typename Type_, vTensor::Access::Flags kAccess_, typename>
inline vTensor::Future<Type, kAccess>::Future(
    Future<Type_, kAccess_>&& future)
  : tensor_(std::move(future.tensor_)) {
  future.tensor_ = nullptr;
}

template<typename Type, vTensor::Access::Flags kAccess>
template<typename Type_, vTensor::Access::Flags kAccess_, typename>
inline vTensor::Future<Type, kAccess>&
vTensor::Future<Type, kAccess>::operator=(
    Future<Type_, kAccess_>&& future) & {
  tensor_ = std::move(future.tensor_);
  future.tensor_ = nullptr;
  return *this;
}

template<typename Type, vTensor::Access::Flags kAccess>
inline vTensor::Future<Type, kAccess>::~Future() {
  if (tensor_ && (kAccess & vTensor::Access::Write)) {
    // tensor->upload_eagerly();
  }
}

template<typename Type, vTensor::Access::Flags kAccess>
inline typename vTensor::Future<Type, kAccess>::Data
vTensor::Future<Type, kAccess>::wait() const & {
  TORCH_CHECK(
      tensor_,
      "vTensor::Future is in an invalid state!  "
      "Potential reason: This future is moved from.");

  api::Resource::Buffer& buffer =
      tensor_->staging_ ?
          tensor_->staging_ :
          tensor_->buffer_;

  return buffer.memory.template map<Type, kAccess>();
}

template<typename Type>
inline vTensor::Future<Type, vTensor::Access::Read> vTensor::host() const & {
  return Future<Type, vTensor::Access::Read>(host());
}

template<typename Type, vTensor::Access::Flags kAccess>
inline vTensor::Future<Type, kAccess> vTensor::host() & {
  return Future<Type, kAccess>(host(kAccess));
}

} // namespace ops
} // namespace vulkan
} // namespace native
} // namespace at
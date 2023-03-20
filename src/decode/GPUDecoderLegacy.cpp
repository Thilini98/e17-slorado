#include "GPUDecoder.h"

#include "Decoder.h"

#include <c10/cuda/CUDAGuard.h>
#include <torch/torch.h>
#include "error.h"

#ifdef USE_KOI
#include <cuda_runtime.h>
extern "C" {
#include "koi.h"
}

static inline void gpu_assert(const char* file, uint64_t line) {
    cudaError_t code = cudaGetLastError();
    if (code != cudaSuccess) {
        fprintf(stderr, "[%s::ERROR]\033[1;31m Cuda error: %s \n in file : %s line number : %lu\033[0m\n",
                __func__, cudaGetErrorString(code), file, line);
        if (code == cudaErrorLaunchTimeout) {
            ERROR("%s", "The kernel timed out. You have to first disable the cuda "
                        "time out.");
            fprintf(
                stderr,
                "On Ubuntu do the following\nOpen the file /etc/X11/xorg.conf\nYou "
                "will have a section about your NVIDIA device. Add the following "
                "line to it.\nOption \"Interactive\" \"0\"\nIf you do not have a "
                "section about your NVIDIA device in /etc/X11/xorg.conf or you do "
                "not have a file named /etc/X11/xorg.conf, run the command sudo "
                "nvidia-xconfig to generate a xorg.conf file and do as above.\n\n");
        }
        exit(-1);
    }
}

#define CUDA_CHK()                                                             \
    { gpu_assert(__FILE__, __LINE__); }
#endif

std::vector<DecodedChunk> GPUDecoder::beam_search(const torch::Tensor &scores,
                                                  int num_chunks,
                                                  const DecoderOptions &options,
                                                  std::string &device) {
    return cpu_part(gpu_part(scores, num_chunks, options, device));
}
torch::Tensor GPUDecoder::gpu_part(torch::Tensor scores, int num_chunks, DecoderOptions options, std::string device) {
#ifdef USE_KOI
    // nvtx3::scoped_range loop{"gpu_decode"};
    long int N = scores.sizes()[0];
    long int T = scores.sizes()[1];
    long int C = scores.sizes()[2];

    auto tensor_options_int32 = torch::TensorOptions()
                                        .dtype(torch::kInt32)
                                        .device(scores.device())
                                        .requires_grad(false);

    auto tensor_options_int8 =
            torch::TensorOptions().dtype(torch::kInt8).device(scores.device()).requires_grad(false);

    if (!initialized) {
        chunks = torch::empty({N, 4}, tensor_options_int32);
        chunks.index({torch::indexing::Slice(), 0}) = torch::arange(0, int(T * N), int(T));
        chunks.index({torch::indexing::Slice(), 2}) = torch::arange(0, int(T * N), int(T));
        chunks.index({torch::indexing::Slice(), 1}) = int(T);
        chunks.index({torch::indexing::Slice(), 3}) = 0;

        chunk_results = torch::empty({N, 8}, tensor_options_int32);

        chunk_results = chunk_results.contiguous();

        aux = torch::empty(N * (T + 1) * (C + 4 * options.beam_width), tensor_options_int8);
        path = torch::zeros(N * (T + 1), tensor_options_int32);

        moves_sequence_qstring = torch::zeros({3, N * T}, tensor_options_int8);

        initialized = true;
    }

    moves_sequence_qstring.index({torch::indexing::Slice()}) = 0.0;
    auto moves = moves_sequence_qstring[0];
    auto sequence = moves_sequence_qstring[1];
    auto qstring = moves_sequence_qstring[2];

    c10::cuda::CUDAGuard device_guard(scores.device());
    int cuda_device_num = std::stoi(device.substr(5));
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, cuda_device_num);
    CUDA_CHK();
    cudaSetDevice(cuda_device_num);
    CUDA_CHK();
    int cuda_device_num_current=-1;
    cudaGetDevice(&cuda_device_num_current);
    CUDA_CHK();
    LOG_TRACE("Running on %s (device id %d)", prop.name, cuda_device_num_current);

    host_back_guide_step(chunks.data_ptr(), chunk_results.data_ptr(), N, scores.data_ptr(), C,
                         aux.data_ptr(), path.data_ptr(), moves.data_ptr(), NULL,
                         sequence.data_ptr(), qstring.data_ptr(), options.q_scale, options.q_shift,
                         options.beam_width, options.beam_cut, options.blank_score);

    host_beam_search_step(chunks.data_ptr(), chunk_results.data_ptr(), N, scores.data_ptr(), C,
                          aux.data_ptr(), path.data_ptr(), moves.data_ptr(), NULL,
                          sequence.data_ptr(), qstring.data_ptr(), options.q_scale, options.q_shift,
                          options.beam_width, options.beam_cut, options.blank_score);

    host_compute_posts_step(chunks.data_ptr(), chunk_results.data_ptr(), N, scores.data_ptr(), C,
                            aux.data_ptr(), path.data_ptr(), moves.data_ptr(), NULL,
                            sequence.data_ptr(), qstring.data_ptr(), options.q_scale,
                            options.q_shift, options.beam_width, options.beam_cut,
                            options.blank_score);

    host_run_decode(chunks.data_ptr(), chunk_results.data_ptr(), N, scores.data_ptr(), C,
                    aux.data_ptr(), path.data_ptr(), moves.data_ptr(), NULL, sequence.data_ptr(),
                    qstring.data_ptr(), options.q_scale, options.q_shift, options.beam_width,
                    options.beam_cut, options.blank_score, options.move_pad);

    return moves_sequence_qstring.reshape({3, N, -1}).to(torch::kCPU);
#else
    return torch::empty({1});
#endif
}

std::vector<DecodedChunk> GPUDecoder::cpu_part(torch::Tensor moves_sequence_qstring_cpu) {
#ifdef USE_KOI
    // nvtx3::scoped_range loop{"cpu_decode"};
    assert(moves_sequence_qstring_cpu.device() == torch::kCPU);
    auto moves_cpu = moves_sequence_qstring_cpu[0];
    auto sequence_cpu = moves_sequence_qstring_cpu[1];
    auto qstring_cpu = moves_sequence_qstring_cpu[2];
    int N = moves_cpu.size(0);
    int T = moves_cpu.size(1);

    std::vector<DecodedChunk> called_chunks;

    for (int idx = 0; idx < N; idx++) {
        std::vector<uint8_t> mov((uint8_t *)moves_cpu[idx].data_ptr(),
                                 (uint8_t *)moves_cpu[idx].data_ptr() + T);
        auto num_bases = moves_cpu[idx].sum().item<int>();
        std::string seq((char *)sequence_cpu[idx].data_ptr(),
                        (char *)sequence_cpu[idx].data_ptr() + num_bases);
        std::string qstr((char *)qstring_cpu[idx].data_ptr(),
                         (char *)qstring_cpu[idx].data_ptr() + num_bases);

        called_chunks.emplace_back(DecodedChunk{std::move(seq), std::move(qstr), std::move(mov)});
    }

    return called_chunks;
#else
    return std::vector<DecodedChunk>();
#endif
}

// int GPUDecoder::get_cuda_device_id_from_device(const c10::Device& device) {
//     if (!device.is_cuda() || !device.has_index()) {
//         std::stringstream ss;
//         ss << "Unable to extract CUDA device ID from device " << device;
//         throw std::runtime_error(ss.str());
//     }

//     return device.index();
// }

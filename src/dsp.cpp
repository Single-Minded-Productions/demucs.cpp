#include "dsp.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <libnyquist/Common.h>
#include <libnyquist/Decoders.h>
#include <libnyquist/Encoders.h>
#include <memory>
#include <string>
#include <unsupported/Eigen/FFT>
#include <vector>

using namespace nqr;

Eigen::MatrixXf demucscpp::load_audio(std::string filename)
{
    // load a wav file with libnyquist
    std::shared_ptr<AudioData> fileData = std::make_shared<AudioData>();

    NyquistIO loader;

    loader.Load(fileData.get(), filename);

    if (fileData->sampleRate != SUPPORTED_SAMPLE_RATE)
    {
        std::cerr
            << "[ERROR] demucs.cpp only supports the following sample rate (Hz): "
            << SUPPORTED_SAMPLE_RATE << std::endl;
        exit(1);
    }

    std::cout << "Input samples: "
              << fileData->samples.size() / fileData->channelCount << std::endl;
    std::cout << "Length in seconds: " << fileData->lengthSeconds << std::endl;
    std::cout << "Number of channels: " << fileData->channelCount << std::endl;

    if (fileData->channelCount != 2 && fileData->channelCount != 1)
    {
        std::cerr << "[ERROR] demucs.cpp only supports mono and stereo audio"
                  << std::endl;
        exit(1);
    }

    // number of samples per channel
    size_t N = fileData->samples.size() / fileData->channelCount;

    // create a struct to hold two float vectors for left and right channels
    Eigen::MatrixXf ret(2, N);

    if (fileData->channelCount == 1)
    {
        // Mono case
        for (size_t i = 0; i < N; ++i)
        {
            ret(0, i) = fileData->samples[i]; // left channel
            ret(1, i) = fileData->samples[i]; // right channel
        }
    }
    else
    {
        // Stereo case
        for (size_t i = 0; i < N; ++i)
        {
            ret(0, i) = fileData->samples[2 * i];     // left channel
            ret(1, i) = fileData->samples[2 * i + 1]; // right channel
        }
    }

    return ret;
}

// write a function to write a StereoWaveform to a wav file
void demucscpp::write_audio_file(const Eigen::MatrixXf &waveform,
                                 std::string filename)
{
    // create a struct to hold the audio data
    std::shared_ptr<AudioData> fileData = std::make_shared<AudioData>();

    // set the sample rate
    fileData->sampleRate = SUPPORTED_SAMPLE_RATE;

    // set the number of channels
    fileData->channelCount = 2;

    // set the number of samples
    fileData->samples.resize(waveform.cols() * 2);

    // write the left channel
    for (long int i = 0; i < waveform.cols(); ++i)
    {
        fileData->samples[2 * i] = waveform(0, i);
        fileData->samples[2 * i + 1] = waveform(1, i);
    }

    int encoderStatus =
        encode_wav_to_disk({fileData->channelCount, PCM_FLT, DITHER_TRIANGLE},
                           fileData.get(), filename);
    std::cout << "Encoder Status: " << encoderStatus << std::endl;
}

// forward declaration of inner stft
void stft_inner(struct demucscpp::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg);

void istft_inner(struct demucscpp::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg);

// reflect padding
void pad_signal(struct demucscpp::stft_buffers &stft_buf)
{
    // copy from stft_buf.padded_waveform_mono_in+pad into stft_buf.pad_start,
    // stft_buf.pad_end
    std::copy_n(stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad,
                stft_buf.pad, stft_buf.pad_start.begin());
    std::copy_n(stft_buf.padded_waveform_mono_in.end() - 2 * stft_buf.pad,
                stft_buf.pad, stft_buf.pad_end.begin());

    std::reverse(stft_buf.pad_start.begin(), stft_buf.pad_start.end());
    std::reverse(stft_buf.pad_end.begin(), stft_buf.pad_end.end());

    // copy stft_buf.pad_start into stft_buf.padded_waveform_mono_in
    std::copy_n(stft_buf.pad_start.begin(), stft_buf.pad,
                stft_buf.padded_waveform_mono_in.begin());

    // copy stft_buf.pad_end into stft_buf.padded_waveform_mono_in
    std::copy_n(stft_buf.pad_end.begin(), stft_buf.pad,
                stft_buf.padded_waveform_mono_in.end() - stft_buf.pad);
}

Eigen::FFT<float> get_fft_cfg()
{
    Eigen::FFT<float> cfg;

    cfg.SetFlag(Eigen::FFT<float>::Speedy);
    // cfg.SetFlag(Eigen::FFT<float>::HalfSpectrum);
    // cfg.SetFlag(Eigen::FFT<float>::Unscaled);

    return cfg;
}

void demucscpp::stft(struct stft_buffers &stft_buf)
{
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        Eigen::VectorXf row_vec = stft_buf.waveform.row(channel);

        std::copy_n(row_vec.data(), row_vec.size(),
                    stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad);

        // apply padding equivalent to center padding with center=True
        // in torch.stft:
        // https://pytorch.org/docs/stable/generated/torch.stft.html

        // reflect pads stft_buf.padded_waveform_mono in-place
        pad_signal(stft_buf);

        // does forward fft on stft_buf.padded_waveform_mono, stores spectrum in
        // complex_spec_mono
        stft_inner(stft_buf, cfg);

        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                stft_buf.spec(channel, i, j) = stft_buf.complex_spec_mono[j][i];
            }
        }
    }
}

void demucscpp::istft(struct stft_buffers &stft_buf)
{
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        // Populate the nested vectors
        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                stft_buf.complex_spec_mono[j][i] = stft_buf.spec(channel, i, j);
            }
        }

        // does inverse fft on stft_buf.complex_spec_mono, stores waveform in
        // padded_waveform_mono
        istft_inner(stft_buf, cfg);

        // copies waveform_mono into stft_buf.waveform past first pad samples
        stft_buf.waveform.row(channel) = Eigen::Map<Eigen::MatrixXf>(
            stft_buf.padded_waveform_mono_out.data() + stft_buf.pad, 1,
            stft_buf.padded_waveform_mono_out.size() - FFT_WINDOW_SIZE);
    }
}

void stft_inner(struct demucscpp::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg)
{
    int frame_idx = 0;

    // Loop over the waveform with a stride of hop_size
    for (std::size_t start = 0;
         start <=
         stft_buf.padded_waveform_mono_in.size() - demucscpp::FFT_WINDOW_SIZE;
         start += demucscpp::FFT_HOP_SIZE)
    {
        // Apply window and run FFT
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE; ++i)
        {
            stft_buf.windowed_waveform_mono[i] =
                stft_buf.padded_waveform_mono_in[start + i] *
                stft_buf.window[i];
        }
        cfg.fwd(stft_buf.complex_spec_mono[frame_idx],
                stft_buf.windowed_waveform_mono);
        // now scale stft_buf.complex_spec_mono[frame_idx] by 1.0f /
        // sqrt(float(FFT_WINDOW_SIZE)))

        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                1.0f / sqrt(float(demucscpp::FFT_WINDOW_SIZE));
        }
        frame_idx++;
    }
}

void istft_inner(struct demucscpp::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg)
{
    // clear padded_waveform_mono
    std::fill(stft_buf.padded_waveform_mono_out.begin(),
              stft_buf.padded_waveform_mono_out.end(), 0.0f);

    // Loop over the input with a stride of (hop_size)
    for (int start = 0; start < stft_buf.nb_frames * demucscpp::FFT_HOP_SIZE;
         start += demucscpp::FFT_HOP_SIZE)
    {
        int frame_idx = start / demucscpp::FFT_HOP_SIZE;
        // undo sqrt(nfft) scaling
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                sqrt(float(demucscpp::FFT_WINDOW_SIZE));
        }
        // Run iFFT
        cfg.inv(stft_buf.windowed_waveform_mono,
                stft_buf.complex_spec_mono[frame_idx]);

        // Apply window and add to output
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE; ++i)
        {
            // x[start+i] is the sum of squared window values
            // https://github.com/librosa/librosa/blob/main/librosa/core/spectrum.py#L613
            // 1e-8f is a small number to avoid division by zero
            stft_buf.padded_waveform_mono_out[start + i] +=
                stft_buf.windowed_waveform_mono[i] * stft_buf.window[i] * 1.0f /
                float(demucscpp::FFT_WINDOW_SIZE) /
                (stft_buf.normalized_window[start + i] + 1e-8f);
        }
    }
}

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <complex>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using Complex = std::complex<float>;
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

std::vector<float> exercise(int order)
{
    const int count = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<Complex> input(count), transformed(count), restored(count);
    for (int i = 0; i < count; ++i)
        input[i] = { static_cast<float>((i * 7) % 17 - 8) / 16.0f,
                     static_cast<float>((i * 11) % 13 - 6) / 16.0f };
    fft.perform(input.data(), transformed.data(), false);
    for (int k = 0; k < count; ++k)
    {
        std::complex<double> expected{};
        for (int n = 0; n < count; ++n)
            expected += static_cast<std::complex<double>>(input[n]) * std::polar(1.0,
                -juce::MathConstants<double>::twoPi * n * k / count);
        require(std::abs(static_cast<std::complex<double>>(transformed[k]) - expected) < 0.001,
            "complex forward transform disagrees with independent DFT");
    }
    fft.perform(transformed.data(), restored.data(), true);
    for (int i = 0; i < count; ++i)
        require(std::abs(restored[i] - input[i]) < 0.00002f, "complex inverse roundtrip");

    std::vector<float> real(2 * count), original(count), magnitudes(2 * count);
    for (int i = 0; i < count; ++i) real[i] = original[i] = input[i].real();
    magnitudes = real;
    fft.performRealOnlyForwardTransform(real.data());
    for (int k = 0; k <= count / 2; ++k)
    {
        std::complex<double> expected{};
        for (int n = 0; n < count; ++n)
            expected += static_cast<double>(original[n]) * std::polar(1.0,
                -juce::MathConstants<double>::twoPi * n * k / count);
        require(std::abs(std::complex<double>(real[2 * k], real[2 * k + 1]) - expected) < 0.001,
            "real forward transform disagrees with independent DFT");
    }
    fft.performFrequencyOnlyForwardTransform(magnitudes.data(), true);
    for (int k = 0; k <= count / 2; ++k)
        require(std::abs(magnitudes[k] - std::abs(Complex(real[2 * k], real[2 * k + 1]))) < 0.0001f,
            "frequency-only transform preserves magnitudes");
    auto result = real;
    result.insert(result.end(), magnitudes.begin(), magnitudes.begin() + count / 2 + 1);
    fft.performRealOnlyInverseTransform(real.data());
    for (int i = 0; i < count; ++i)
        require(std::abs(real[i] - original[i]) < 0.00002f, "real inverse roundtrip");
    for (const auto value : transformed) { result.push_back(value.real()); result.push_back(value.imag()); }
    return result;
}
}

int main(int argc, char** argv)
{
    try
    {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::array<std::exception_ptr, 4> failures;
        std::array<std::vector<float>, 4> outputs;
        std::vector<std::thread> workers;
        for (size_t i = 0; i < outputs.size(); ++i)
            workers.emplace_back([&, i]
            {
                ++ready;
                while (!go.load()) std::this_thread::yield();
                try { outputs[i] = exercise(6); } catch (...) { failures[i] = std::current_exception(); }
            });
        while (ready.load() != static_cast<int>(outputs.size())) std::this_thread::yield();
        go = true;
        for (auto& worker : workers) worker.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
        for (const auto& output : outputs) require(output == outputs[0], "concurrent first-use output differs");
        auto result = outputs[0];
        for (const int order : {1, 3, 8})
        {
            auto values = exercise(order);
            result.insert(result.end(), values.begin(), values.end());
        }
        if (argc == 2)
        {
            std::ofstream out(argv[1], std::ios::binary | std::ios::out);
            out.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size() * sizeof(float)));
            require(out.good(), "write deterministic numeric reference");
        }
        std::cout << "FFT concurrent first-use, independent DFT, complex/real inverse and magnitudes passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

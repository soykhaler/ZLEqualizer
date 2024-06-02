// Copyright (C) 2024 - zsliu98
// This file is part of ZLEqualizer
//
// ZLEqualizer is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
// ZLEqualizer is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with ZLEqualizer. If not, see <https://www.gnu.org/licenses/>.

#include "conflict_analyzer.hpp"

namespace zlFFT {
    template<typename FloatType>
    ConflictAnalyzer<FloatType>::ConflictAnalyzer()
        : Thread("conflict_analyzer") {
        std::fill(mainDB.begin(), mainDB.end(), -144.f);
        std::fill(refDB.begin(), refDB.end(), -144.f);
        syncAnalyzer.setDecayRate(0, 0.985f);
        syncAnalyzer.setDecayRate(1, 0.985f);
    }

    template<typename FloatType>
    ConflictAnalyzer<FloatType>::~ConflictAnalyzer() {
        if (isThreadRunning()) {
            stopThread(-1);
        }
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::prepare(const juce::dsp::ProcessSpec &spec) {
        syncAnalyzer.prepare(spec);
        mainBuffer.setSize(static_cast<int>(spec.numChannels), static_cast<int>(spec.maximumBlockSize));
        refBuffer.setSize(static_cast<int>(spec.numChannels), static_cast<int>(spec.maximumBlockSize));
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::setON(const bool x) {
        syncAnalyzer.reset();
        isON.store(x);
        toReset.store(true);
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::pushMainBuffer(juce::AudioBuffer<FloatType> &buffer) {
        mainBuffer.makeCopyOf(buffer, true);
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::pushRefBuffer(juce::AudioBuffer<FloatType> &buffer) {
        refBuffer.makeCopyOf(buffer, true);
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::process() {
        if (isON.load()) {
            syncAnalyzer.process(mainBuffer, refBuffer);
            if (!isConflictReady.load()) {
                triggerAsyncUpdate();
            }
        }
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::run() {
        juce::ScopedNoDenormals noDenormals;
        while (!threadShouldExit()) {
            syncAnalyzer.run();
            const auto &mainDBAtomic = syncAnalyzer.getInterplotDBs(0);
            const auto &refDBAtomic = syncAnalyzer.getInterplotDBs(1);
            for (size_t i = 0; i < mainDB.size(); ++i) {
                mainDB[i] = mainDBAtomic[i].load();
                refDB[i] = refDBAtomic[i].load();
            }
            const auto mainM = std::reduce(mainDB.begin(), mainDB.end()) / static_cast<float>(mainDB.size());
            const auto refM = std::reduce(refDB.begin(), refDB.end()) / static_cast<float>(refDB.size());
            const auto threshold = juce::jmin(static_cast<float>(strength.load()) * (mainM + refM), 0.f);

            if (toReset.exchange(false)) {
                std::fill(conflicts.begin(), conflicts.end(), 0.f);
            }
            for (size_t i = 0; i < conflicts.size(); ++i) {
                const auto fftIdx = 4 * i;
                const auto dB1 = (mainDB[fftIdx] + mainDB[fftIdx + 1] + mainDB[fftIdx + 2] + mainDB[fftIdx + 3]) *
                                 .25f;
                const auto dB2 = (refDB[fftIdx] + refDB[fftIdx + 1] + refDB[fftIdx + 2] + refDB[fftIdx + 3]) * .25f;
                const auto dBMin = juce::jmin(dB1, dB2, 0.001f);
                conflicts[i] = juce::jmax(conflicts[i] * .98f,
                                          (dBMin - threshold) / (0.001f - threshold));
            }
            for (size_t i = 1; i < conflicts.size() - 1; ++i) {
                conflicts[i] = conflicts[i] * .75f + (conflicts[i - 1] + conflicts[i + 1]) * .125f;
            }

            // calculate the conflict portion
            const auto scale = static_cast<float>(conflictScale.load());
            for (size_t i = 0; i < conflicts.size(); ++i) {
                conflictsP[i] = conflicts[i] * scale;
                if (conflictsP[i].load() >= 0.01) {
                    conflictsP[i].store(juce::jmin(.75f, conflictsP[i].load()));
                } else {
                    conflictsP[i].store(-1.f);
                }
            }
            isConflictReady.store(true);
            const auto flag = wait(-1);
            juce::ignoreUnused(flag);
        }
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::drawGradient(juce::Graphics &g, juce::Rectangle<float> bound) {
        // calculate gradient
        gradient.point1 = juce::Point<float>(x1.load(), 0.f);
        gradient.point2 = juce::Point<float>(x2.load(), 0.f);
        gradient.isRadial = false;
        gradient.clearColours();

        gradient.addColour(0.0,
                           gColour.withMultipliedAlpha(juce::jmax(conflictsP.front().load(), 0.f)));
        gradient.addColour(1.0,
                           gColour.withMultipliedAlpha(juce::jmax(conflictsP.back().load(), 0.f)));
        for (size_t i = 1; i < conflictsP.size() - 1; ++i) {
            if (conflictsP[i + 1] > 0 || conflictsP[i - 1] > 0) {
                const auto p = (static_cast<double>(i) + 0.5) / static_cast<double>(conflictsP.size());
                const auto rectColour = gColour.withMultipliedAlpha(juce::jmax(conflictsP[i].load(), 0.f));
                gradient.addColour(p, rectColour);
            }
        }

        g.setGradientFill(gradient);
        g.fillRect(bound);
        isConflictReady.store(false);
    }

    template<typename FloatType>
    void ConflictAnalyzer<FloatType>::handleAsyncUpdate() {
        notify();
    }

    template
    class ConflictAnalyzer<float>;

    template
    class ConflictAnalyzer<double>;
} // zlFFT

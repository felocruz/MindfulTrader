#pragma once

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <Eigen/Dense>

namespace MindfulTrader {

    /**
     * @brief Observation Compressor (PCA Engine)
     * 
     * Performs real-time Principal Component Analysis (PCA) to compress the
    * 16D Market Physics vector into 8D orthogonal features for HMM consumption.
     * 
     * Rationale:
     * - Removes multicollinearity (noise) between indicators.
     * - Reduces "Curse of Dimensionality" for the HMM.
     * - "Principal Component 1" naturally captures "Market Beta" (Global Trend).
     * 
     * Implementation:
     * - Maintains a rolling window of recent observations (N=500).
     * - Computes Covariance Matrix and Eigendecomposition on every update (Institutional Grade).
     * - Projects the *current* observation into the 8D latent space.
     */
    class ObservationCompressor {
    public:
        // Input: canonical 16D ObservationData vector
        static constexpr int INPUT_DIM = 16;
        // Output: 8D Latent Vector
        static constexpr int OUTPUT_DIM = 8;
        // Lookback: 500 bars
        static constexpr int WINDOW_SIZE = 500;

        ObservationCompressor() : m_count(0) {
            m_history.setZero();
            m_centered.setZero();
            m_cov.setZero();
            m_mean.setZero();
        }

        /**
         * @brief Adds a raw observation to the rolling history buffer.
         */
        void AddHistory(const std::array<float, INPUT_DIM>& obs) {
            if (WINDOW_SIZE > 1) {
                m_history.block(0, 0, WINDOW_SIZE - 1, INPUT_DIM) = 
                    m_history.block(1, 0, WINDOW_SIZE - 1, INPUT_DIM);
            }
            
            for (int i = 0; i < INPUT_DIM; ++i) {
                m_history(WINDOW_SIZE - 1, i) = obs[i];
            }

            if (m_count < WINDOW_SIZE) m_count++;
        }

        /**
         * @brief Projects a compressed 8D vector from the current raw input.
         * Uses zero-allocation robust Eigen patterns.
         */
        std::array<float, OUTPUT_DIM> ComputePrincipalComponents(const std::array<float, INPUT_DIM>& currentObs) {
            std::array<float, OUTPUT_DIM> result = {0.0f};

            if (m_count < INPUT_DIM + 2) return result;

            // 1. Snapshot valid history (Expression only, no alloc)
            auto X = m_history.bottomRows(m_count);

            // 2. Center Data (into pre-allocated member)
            m_mean = X.colwise().mean();
            
            // Center into pre-allocated workspace
            m_centered.topRows(m_count) = X.rowwise() - m_mean;

            // 3. Covariance (noalias ensures no temporary)
            auto centeredBlock = m_centered.topRows(m_count);
            // Cast to float divisor explicitly
            m_cov.noalias() = (centeredBlock.adjoint() * centeredBlock) / static_cast<float>(m_count - 1);

            // 4. Eigen Solve (reuses internal buffer)
            m_solver.compute(m_cov);

            // 5. Projection (Direct calculation without W matrix)
            Eigen::Matrix<float, 1, INPUT_DIM> x_vec;
            for(int i=0; i<INPUT_DIM; ++i) x_vec(i) = currentObs[i];
            
            x_vec -= m_mean; 

            for(int k = 0; k < OUTPUT_DIM; ++k) {
                int eigenIndex = INPUT_DIM - 1 - k;
                result[k] = x_vec.dot(m_solver.eigenvectors().col(eigenIndex));
            }

            return result;
        }

        bool IsReady() const { return m_count >= WINDOW_SIZE; }

    private:
        // Memory Layout: All Fixed Size -> Zero Heap Allocations
        Eigen::Matrix<float, WINDOW_SIZE, INPUT_DIM> m_history;
        Eigen::Matrix<float, WINDOW_SIZE, INPUT_DIM> m_centered;
        Eigen::Matrix<float, INPUT_DIM, INPUT_DIM> m_cov;
        Eigen::Matrix<float, 1, INPUT_DIM> m_mean;
        
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix<float, INPUT_DIM, INPUT_DIM>> m_solver;
        
        int m_count;
    };
}

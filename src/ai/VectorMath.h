#ifndef VECTORMATH_H
#define VECTORMATH_H

#include <QVector>
#include <cmath>

namespace VectorMath {

inline float cosineSimilarity(const QVector<float> &a, const QVector<float> &b) {
  if (a.size() != b.size() || a.isEmpty())
    return 0.0f;
  float dot = 0, normA = 0, normB = 0;
  for (int i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }
  float denom = std::sqrt(normA) * std::sqrt(normB);
  return denom > 0 ? dot / denom : 0.0f;
}

} // namespace VectorMath

#endif // VECTORMATH_H

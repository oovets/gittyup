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

// Plain dot product. For two unit-length vectors this equals cosine
// similarity, but skips the two square roots per comparison.
inline float dot(const QVector<float> &a, const QVector<float> &b) {
  if (a.size() != b.size() || a.isEmpty())
    return 0.0f;
  float d = 0;
  for (int i = 0; i < a.size(); ++i)
    d += a[i] * b[i];
  return d;
}

// Scale a vector to unit length in place. Zero vectors are left untouched.
inline void normalize(QVector<float> &v) {
  float norm = 0;
  for (int i = 0; i < v.size(); ++i)
    norm += v[i] * v[i];
  norm = std::sqrt(norm);
  if (norm <= 0)
    return;
  const float inv = 1.0f / norm;
  for (int i = 0; i < v.size(); ++i)
    v[i] *= inv;
}

inline QVector<float> normalized(QVector<float> v) {
  normalize(v);
  return v;
}

} // namespace VectorMath

#endif // VECTORMATH_H

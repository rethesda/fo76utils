
#include "common.hpp"
#include "plot3d.hpp"
#include "fp32vec4.hpp"

#include <algorithm>

#if 0
#  define FAST_ENVIRONMENT_MAP  1
#endif

// vertex coordinates closer than this to exact integers are rounded
#ifndef VERTEX_XY_SNAP
#  define VERTEX_XY_SNAP  0.03125f
#endif

const float Plot3D_TriShape::defaultLightingPolynomial[6] =
{
  0.672235f, 0.997428f, 0.009355f, -0.771812f, 0.108711f, 0.369682f
};

void Plot3D_TriShape::calculateWaterUV(
    const NIFFile::NIFVertexTransform& modelTransform)
{
  textureOffsetU = 0.0f;
  textureOffsetV = 0.0f;
  textureScaleU = 1.0f;
  textureScaleV = 1.0f;
  float   u0 = 0.0f;
  float   v0 = 0.0f;
  for (size_t i = 0; i < vertexCnt; i++)
  {
    NIFFile::NIFVertex  v(vertexData[i]);
    modelTransform.transformXYZ(v.x, v.y, v.z);
    float   tmpU = v.x * (2.0f / 4096.0f);
    float   tmpV = v.y * (2.0f / 4096.0f);
    if (!i)
    {
      u0 = float(roundFloat(tmpU));
      v0 = float(roundFloat(tmpV));
    }
    vertexBuf[i].u = convertToFloat16(tmpU - u0);
    vertexBuf[i].v = convertToFloat16(tmpV - v0);
  }
}

size_t Plot3D_TriShape::transformVertexData(
    const NIFFile::NIFVertexTransform& modelTransform,
    const NIFFile::NIFVertexTransform& viewTransform,
    float& lightX, float& lightY, float& lightZ)
{
  if (vertexBuf.size() < vertexCnt)
    vertexBuf.resize(vertexCnt);
  triangleBuf.clear();
  if (triangleBuf.capacity() < triangleCnt)
    triangleBuf.reserve(triangleCnt);
  NIFFile::NIFVertexTransform vt(vertexTransform);
  vt *= modelTransform;
  vt *= viewTransform;
  if (flags & 0x08)                     // decal
    vt.offsZ = vt.offsZ - 0.0625f;
  NIFFile::NIFBounds  b;
  for (size_t i = 0; i < vertexCnt; i++)
  {
    NIFFile::NIFVertex& v = vertexBuf[i];
    v = vertexData[i];
    vt.transformXYZ(v.x, v.y, v.z);
    b += v;
  }
  if (b.xMin >= (float(width) - 0.5f) || b.xMax < -0.5f ||
      b.yMin >= (float(height) - 0.5f) || b.yMax < -0.5f ||
      b.zMin >= 16777216.0f || b.zMax < 0.0f)
  {
    return 0;
  }
  viewTransform.rotateXYZ(lightX, lightY, lightZ);
  for (size_t i = 0; i < vertexCnt; i++)
  {
    NIFFile::NIFVertex& v = vertexBuf[i];
    float   x, y;
#ifdef VERTEX_XY_SNAP
    x = float(roundFloat(v.x));
    y = float(roundFloat(v.y));
    v.x = (float(std::fabs(v.x - x)) < VERTEX_XY_SNAP ? x : v.x);
    v.y = (float(std::fabs(v.y - y)) < VERTEX_XY_SNAP ? y : v.y);
#endif
    x = v.normalX;
    y = v.normalY;
    float   z = v.normalZ;
    v.normalX = (x * vt.rotateXX) + (y * vt.rotateXY) + (z * vt.rotateXZ);
    v.normalY = (x * vt.rotateYX) + (y * vt.rotateYY) + (z * vt.rotateYZ);
    v.normalZ = (x * vt.rotateZX) + (y * vt.rotateZY) + (z * vt.rotateZZ);
    if (flags & 0x20)                   // tree: ignore vertex alpha
      v.vertexColor = v.vertexColor | 0xFF000000U;
  }
  if (flags & 0x02)
    calculateWaterUV(modelTransform);
  for (size_t i = 0; i < triangleCnt; i++)
  {
    if (triangleData[i].v0 >= vertexCnt || triangleData[i].v1 >= vertexCnt ||
        triangleData[i].v2 >= vertexCnt)
    {
      continue;
    }
    const NIFFile::NIFVertex& v0 = vertexBuf[triangleData[i].v0];
    const NIFFile::NIFVertex& v1 = vertexBuf[triangleData[i].v1];
    const NIFFile::NIFVertex& v2 = vertexBuf[triangleData[i].v2];
    if (!(flags & 0x10))
    {
      if (((v1.x - v0.x) * (v2.y - v0.y)) > ((v2.x - v0.x) * (v1.y - v0.y)))
        continue;               // cull if vertices are not in CCW order
    }
    int     x0 = int(v0.x);
    int     y0 = int(v0.y);
    int     x1 = int(v1.x);
    int     y1 = int(v1.y);
    int     x2 = int(v2.x);
    int     y2 = int(v2.y);
    if ((x0 < 0 && x1 < 0 && x2 < 0) ||
        (x0 >= width && x1 >= width && x2 >= width) ||
        (y0 < 0 && y1 < 0 && y2 < 0) ||
        (y0 >= height && y1 >= height && y2 >= height) ||
        (v0.z < 0.0f && v1.z < 0.0f && v2.z < 0.0f))
    {
      continue;
    }
#ifdef VERTEX_XY_SNAP
    if (x0 == x1 && x0 == x2)
    {
      if ((v0.x - float(x0)) != 0.0f && (v1.x - float(x1)) != 0.0f &&
          (v2.x - float(x2)) != 0.0f)
      {
        continue;
      }
    }
    if (y0 == y1 && y0 == y2)
    {
      if ((v0.y - float(y0)) != 0.0f && (v1.y - float(y1)) != 0.0f &&
          (v2.y - float(y2)) != 0.0f)
      {
        continue;
      }
    }
#endif
    Triangle  tmp;
    tmp.z = v0.z + v1.z + v2.z;
    tmp.n = (unsigned int) i;
    triangleBuf.push_back(tmp);
  }
  std::stable_sort(triangleBuf.begin(), triangleBuf.end());
  return triangleBuf.size();
}

inline int Plot3D_TriShape::getLightLevel(float d) const
{
  int     x = roundFloat(d * 32768.0f);
  int     xf = x & 0xFF;
  x = int((unsigned int) x >> 8);
  int     y0 = lightTable[x & 0x01FF];
  x++;
  return (((y0 << 8) + ((lightTable[x & 0x01FF] - y0) * xf) + 32768) >> 16);
}

inline int Plot3D_TriShape::normalMap(
    float& normalX, float& normalY, float& normalZ, unsigned int n) const
{
  float   x = float(int(n & 0xFF)) * (1.0f / 127.5f) - 1.0f;
  float   y = float(int((n >> 8) & 0xFF)) * (1.0f / 127.5f) - 1.0f;
  float   tmpX = normalX;
  float   tmpY = normalY;
  float   tmpZ = normalZ;
  float   tmp = (tmpX * tmpX) + (tmpY * tmpY) + (tmpZ * tmpZ);
  // calculate Z normal
  float   z = 1.0f - ((x * x) + (y * y));
  if (BRANCH_EXPECT((tmp > 0.0f && z > 0.0f), true))
    z = float(std::sqrt(z / tmp));
  else
    z = 0.0f;
  tmpX = (x * bitangentX) + (y * tangentX) + (z * tmpX);
  tmpY = (x * bitangentY) + (y * tangentY) + (z * tmpY);
  tmpZ = (x * bitangentZ) + (y * tangentZ) + (z * tmpZ);
  // normalize
  tmp = (tmpX * tmpX) + (tmpY * tmpY) + (tmpZ * tmpZ);
  if (BRANCH_EXPECT((tmp > 0.0f), true))
    tmp = 1.0f / float(std::sqrt(tmp));
  if (invNormals)
    tmp = -tmp;
  tmpX *= tmp;
  tmpY *= tmp;
  tmpZ *= tmp;
  normalX = tmpX;
  normalY = tmpY;
  normalZ = tmpZ;
  return getLightLevel((tmpX * light_X) + (tmpY * light_Y) + (tmpZ * light_Z));
}

inline unsigned int Plot3D_TriShape::environmentMap(
    float normalX, float normalY, float normalZ,
    int x, int y, unsigned int smoothness) const
{
  float   s = float(int(smoothness * (unsigned int) specularSmoothness))
              * (1.0f / 65025.0f);
  float   m = (1.0f - s) * float(textureE->getMaxMipLevel() + 1);
#ifdef FAST_ENVIRONMENT_MAP
  float   u = float((width >> 1) - x) * envMapUVScale;
  float   v = float((height >> 1) - y) * envMapUVScale;
  if (normalZ < -0.03125f)
  {
    u = u + (normalX * (0.25f / normalZ));
    v = v + (normalY * (0.25f / normalZ));
  }
  else
  {
    u = u - (normalX * 8.0f);
    v = v - (normalY * 8.0f);
  }
  return textureE->cubeMap(0.46875f, u, v, m);
#else
  // view vector
  float   xc = float(x - (width >> 1)) * envMapUVScale;
  float   yc = float(y - (height >> 1)) * envMapUVScale;
  float   zc = 0.25f;
  // reflect
  float   tmp = ((xc * normalX) + (yc * normalY) + (zc * normalZ)) * 2.0f;
  xc = xc - (normalX * tmp);
  yc = yc - (normalY * tmp);
  zc = zc - (normalZ * tmp);
  // inverse rotation by view matrix
  const NIFFile::NIFVertexTransform&  vt = *viewTransformPtr;
  float   tmpX = (xc * vt.rotateXX) + (yc * vt.rotateYX) + (zc * vt.rotateZX);
  float   tmpY = (xc * vt.rotateXY) + (yc * vt.rotateYY) + (zc * vt.rotateZY);
  float   tmpZ = (xc * vt.rotateXZ) + (yc * vt.rotateYZ) + (zc * vt.rotateZZ);
  return textureE->cubeMap(tmpX, tmpY, tmpZ, m);
#endif
}

inline unsigned int Plot3D_TriShape::addReflection(
    unsigned int c, unsigned int e) const
{
  e = multiplyWithLight(e, reflectionLevel);
  unsigned int  rb = (c & 0x00FF00FFU) + (e & 0x00FF00FFU);
  unsigned int  g = (c & 0x0000FF00U) + (e & 0x0000FF00U);
  return (0xFF000000U | (rb & 0x00FF00FFU) | (g & 0x0000FF00U)
          | ((((rb & 0x01000100U) | (g & 0x00010000U)) >> 8) * 0xFFU));
}

inline unsigned int Plot3D_TriShape::addReflectionM(
    unsigned int c, unsigned int e, unsigned int m) const
{
  int     l = int(((unsigned int) reflectionLevel * (m & 0xFFU) + 0x80U) >> 8);
  e = multiplyWithLight(e, l);
  unsigned int  rb = (c & 0x00FF00FFU) + (e & 0x00FF00FFU);
  unsigned int  g = (c & 0x0000FF00U) + (e & 0x0000FF00U);
  return (0xFF000000U | (rb & 0x00FF00FFU) | (g & 0x0000FF00U)
          | ((((rb & 0x01000100U) | (g & 0x00010000U)) >> 8) * 0xFFU));
}

inline unsigned int Plot3D_TriShape::addReflectionR(
    unsigned int c, unsigned int e, unsigned int r) const
{
  unsigned int  l = (unsigned int) reflectionLevel;
  unsigned int  tmpR =
      (((e & 0xFFU) * ((r & 0xFFU) * l) + 0x8000U) >> 16)
      + (c & 0xFFU);
  unsigned int  tmpG =
      ((((e >> 8) & 0xFFU) * (((r >> 8) & 0xFFU) * l) + 0x8000U) >> 16)
      + ((c >> 8) & 0xFFU);
  unsigned int  tmpB =
      ((((e >> 16) & 0xFFU) * (((r >> 16) & 0xFFU) * l) + 0x8000U) >> 16)
      + ((c >> 16) & 0xFFU);
  tmpR = (tmpR < 255U ? tmpR : 255U);
  tmpG = (tmpG < 255U ? tmpG : 255U);
  tmpB = (tmpB < 255U ? tmpB : 255U);
  return (0xFF000000U | tmpR | (tmpG << 8) | (tmpB << 16));
}

void Plot3D_TriShape::drawPixel_Water(Plot3D_TriShape& p,
                                      int x, int y, float txtU, float txtV,
                                      const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  p.bufZ[offs] = z.z;
  unsigned int  c = p.bufRGBW[offs];
  if (BRANCH_EXPECT(!((c + 0x01000000U) & 0xFE000000U), true))
  {
    // convert from R8G8B8 to R5G6B5
    c = c & 0x00FEFEFEU;
    c = c + ((unsigned int) ((x & 1) | (((x ^ y) & 1) << 1)) * 0x00020102U);
    c = c - ((c >> 7) & 0x00020202U);
    c = ((c >> 3) & 0x001FU) | ((c >> 5) & 0x07E0U) | ((c >> 8) & 0xF800U);
  }
  c = c & 0xFFFFU;
  unsigned int  n = 0xFFFF8080U;
  if (p.textureN)
    n = p.textureN->getPixelT(txtU, txtV, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  (void) p.normalMap(normalX, normalY, normalZ, n);
  c = c | ((unsigned int) (roundFloat(normalX * 126.0f) + 128) << 16);
  c = c | ((unsigned int) (roundFloat(normalY * 126.0f) + 128) << 24);
  p.bufRGBW[offs] = c;
}

void Plot3D_TriShape::drawPixel_Debug(Plot3D_TriShape& p,
                                      int x, int y, float txtU, float txtV,
                                      const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = 0U;
  if (BRANCH_EXPECT(p.textureD, true))
  {
    c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
    if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
      c = p.gradientMapAndVColor(c, z.vertexColor);
    if (c < p.alphaThresholdScaled)
      return;
    if (p.debugMode == 5)
      c = 0xFFB8B8B8U;          // full scale with default polynomial
  }
  p.bufZ[offs] = z.z;
  if (!(p.debugMode == 0 || p.debugMode == 3 || p.debugMode == 5))
  {
    if (p.debugMode & 0x80000000U)
    {
      c = p.debugMode;
    }
    else if (p.debugMode == 2)
    {
      c = (unsigned int) roundFloat(z.z * 16.0f);
      c = (c < 0x00FFFFFFU ? c : 0x00FFFFFFU);
    }
    if (p.debugMode != 4)
      c = ((c & 0xFFU) << 16) | (c & 0xFF00U) | ((c >> 16) & 0xFFU);
    p.bufRGBW[offs] = c | 0xFF000000U;
    return;
  }
  unsigned int  n = 0xFFFF8080U;
  if (p.textureN)
  {
    n = p.textureN->getPixelT(txtU * p.textureScaleN,
                              txtV * p.textureScaleN, p.mipLevel);
  }
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  c = multiplyWithLight(c, p.normalMap(normalX, normalY, normalZ, n));
  if (p.debugMode == 3)
  {
    c = (unsigned int) roundFloat(normalX * 127.5f + 127.5f)
        | ((unsigned int) roundFloat(normalY * -127.5f + 127.5f) << 8)
        | ((unsigned int) roundFloat(normalZ * -127.5f + 127.5f) << 16)
        | 0xFF000000U;
  }
  p.bufRGBW[offs] = c;
}

unsigned int Plot3D_TriShape::gradientMapAndVColor(unsigned int c,
                                                   unsigned int vColor) const
{
  if (textureG)
  {
    float   u = float(int((c >> 8) & 0xFFU) * (textureG->getWidth() - 1))
                * (1.0f / 255.0f);
    float   v = float(int(((unsigned int) gradientMapV + vColor + 1U) & 0xFFU)
                      * (textureG->getHeight() - 1)) * (1.0f / 255.0f);
    unsigned int  a = c;
    c = textureG->getPixelB(u, v, 0) & 0x00FFFFFFU;
    if (BRANCH_EXPECT((vColor < 0xFF000000U), false))
      a = ((a >> 24) & 0xFFU) * ((vColor >> 24) & 0xFFU) * 65793U + 0x00800000U;
    return (c | (a & 0xFF000000U));
  }
  unsigned int  r, g, b, a;
  r = ((c & 0xFFU) * vclrTable[vColor & 0xFFU] + 0x4000U) >> 15;
  g = (((c >> 8) & 0xFFU) * vclrTable[(vColor >> 8) & 0xFFU] + 0x4000U) >> 15;
  b = (((c >> 16) & 0xFFU) * vclrTable[(vColor >> 16) & 0xFFU] + 0x4000U) >> 15;
  a = ((c >> 24) & 0xFFU) * ((vColor >> 24) & 0xFFU) * 65793U + 0x00800000U;
  return (r | (g << 8) | (b << 16) | (a & 0xFF000000U));
}

int Plot3D_TriShape::calculateLighting(
    float normalX, float normalY, float normalZ) const
{
  float   tmp = (normalX * normalX) + (normalY * normalY) + (normalZ * normalZ);
  if (BRANCH_EXPECT((tmp > 0.0f), true))
  {
    tmp = 1.0f / float(std::sqrt(tmp));
    normalX *= tmp;
    normalY *= tmp;
    normalZ *= tmp;
  }
  return getLightLevel((normalX * light_X) + (normalY * light_Y)
                       + (normalZ * light_Z));
}

void Plot3D_TriShape::drawPixel_Diffuse(Plot3D_TriShape& p,
                                        int x, int y, float txtU, float txtV,
                                        const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  float   normalX = (!p.invNormals ? z.normalX : -(z.normalX));
  float   normalY = (!p.invNormals ? z.normalY : -(z.normalY));
  float   normalZ = (!p.invNormals ? z.normalZ : -(z.normalZ));
  int     l = p.calculateLighting(normalX, normalY, normalZ);
  p.bufRGBW[offs] = multiplyWithLight(c, l);
}

void Plot3D_TriShape::drawPixel_Normal(Plot3D_TriShape& p,
                                       int x, int y, float txtU, float txtV,
                                       const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  unsigned int  n = p.textureN->getPixelT(txtU * p.textureScaleN,
                                          txtV * p.textureScaleN, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  p.bufRGBW[offs] =
      multiplyWithLight(c, p.normalMap(normalX, normalY, normalZ, n));
}

void Plot3D_TriShape::drawPixel_NormalEnv(Plot3D_TriShape& p,
                                          int x, int y, float txtU, float txtV,
                                          const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  unsigned int  n = p.textureN->getPixelT(txtU * p.textureScaleN,
                                          txtV * p.textureScaleN, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  c = multiplyWithLight(c, p.normalMap(normalX, normalY, normalZ, n));
  p.bufRGBW[offs] =
      p.addReflectionM(c, p.environmentMap(normalX, normalY, normalZ, x, y),
                       n >> 24);
}

void Plot3D_TriShape::drawPixel_NormalEnvM(Plot3D_TriShape& p,
                                           int x, int y, float txtU, float txtV,
                                           const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  unsigned int  n = p.textureN->getPixelT(txtU * p.textureScaleN,
                                          txtV * p.textureScaleN, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  c = multiplyWithLight(c, p.normalMap(normalX, normalY, normalZ, n));
  unsigned int  m = p.textureS->getPixelT(txtU * p.textureScaleS,
                                          txtV * p.textureScaleS, p.mipLevel);
  if (BRANCH_EXPECT((m & 0xFFU), true))
  {
    c = p.addReflectionM(c,
                         p.environmentMap(normalX, normalY, normalZ, x, y), m);
  }
  p.bufRGBW[offs] = c;
}

void Plot3D_TriShape::drawPixel_NormalEnvS(Plot3D_TriShape& p,
                                           int x, int y, float txtU, float txtV,
                                           const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  unsigned int  n = p.textureN->getPixelT(txtU * p.textureScaleN,
                                          txtV * p.textureScaleN, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  c = multiplyWithLight(c, p.normalMap(normalX, normalY, normalZ, n));
  unsigned int  m = p.textureS->getPixelT(txtU * p.textureScaleS,
                                          txtV * p.textureScaleS, p.mipLevel);
  if (BRANCH_EXPECT((m & 0xFFU), true))
  {
    c = p.addReflectionM(c,
                         p.environmentMap(normalX, normalY, normalZ, x, y), m);
  }
  p.bufRGBW[offs] = c;
}

void Plot3D_TriShape::drawPixel_NormalRefl(Plot3D_TriShape& p,
                                           int x, int y, float txtU, float txtV,
                                           const NIFFile::NIFVertex& z)
{
  size_t  offs = size_t(y) * size_t(p.width) + size_t(x);
  unsigned int  c = p.textureD->getPixelT(txtU, txtV, p.mipLevel);
  if (BRANCH_EXPECT((p.textureG || ~(z.vertexColor)), false))
    c = p.gradientMapAndVColor(c, z.vertexColor);
  if (c < p.alphaThresholdScaled)
    return;
  p.bufZ[offs] = z.z;
  unsigned int  n = p.textureN->getPixelT(txtU * p.textureScaleN,
                                          txtV * p.textureScaleN, p.mipLevel);
  unsigned int  r = p.textureR->getPixelT(txtU * p.textureScaleR,
                                          txtV * p.textureScaleR, p.mipLevel);
  unsigned int  s = p.textureS->getPixelT(txtU * p.textureScaleS,
                                          txtV * p.textureScaleS, p.mipLevel);
  float   normalX = z.normalX;
  float   normalY = z.normalY;
  float   normalZ = z.normalZ;
  int     l = p.normalMap(normalX, normalY, normalZ, n);
  if (BRANCH_EXPECT(!(r & 0x00FFFFFFU), false))
  {
    p.bufRGBW[offs] = multiplyWithLight(c, l);
    return;
  }
  unsigned int  r_r = r & 0xFFU;
  unsigned int  r_g = (r >> 8) & 0xFFU;
  unsigned int  r_b = (r >> 16) & 0xFFU;
  unsigned int  m = (r_r > r_g ? r_r : r_g);
  m = (r_b > m ? r_b : m);
  c = multiplyWithLight(c, int(((unsigned int) l * (256U - m) + 128U) >> 8));
  c = p.addReflectionR(c, p.environmentMap(normalX, normalY, normalZ,
                                           x, y, s & 0xFFU), r);
  p.bufRGBW[offs] = c;
}

unsigned int Plot3D_TriShape::interpVertexColors(
    const NIFFile::NIFVertex& v0, const NIFFile::NIFVertex& v1,
    const NIFFile::NIFVertex& v2, float w0, float w1, float w2)
{
  FloatVector4  c0(v0.vertexColor);
  FloatVector4  c1(v1.vertexColor);
  FloatVector4  c2(v2.vertexColor);
  c0 *= w0;
  c1 *= w1;
  c2 *= w2;
  c0 += c1;
  c0 += c2;
  return (unsigned int) c0;
}

inline void Plot3D_TriShape::drawPixel(
    int x, int y, float txtU, float txtV,
    NIFFile::NIFVertex& v, const NIFFile::NIFVertex& v0,
    const NIFFile::NIFVertex& v1, const NIFFile::NIFVertex& v2,
    float w0, float w1, float w2)
{
  if (x < 0 || x >= width || y < 0 || y >= height)
    return;
  if (v.z < 0.0f || bufZ[size_t(y) * size_t(width) + size_t(x)] <= v.z)
    return;
  v.normalX = (v0.normalX * w0) + (v1.normalX * w1) + (v2.normalX * w2);
  v.normalY = (v0.normalY * w0) + (v1.normalY * w1) + (v2.normalY * w2);
  v.normalZ = (v0.normalZ * w0) + (v1.normalZ * w1) + (v2.normalZ * w2);
  unsigned int  tmp = v0.vertexColor & v1.vertexColor & v2.vertexColor;
  if (BRANCH_EXPECT((tmp != 0xFFFFFFFFU), false))
    tmp = interpVertexColors(v0, v1, v2, w0, w1, w2);
  v.vertexColor = tmp;
  drawPixelFunction(*this, x, y, txtU, txtV, v);
}

void Plot3D_TriShape::drawLine(const NIFFile::NIFVertex *v0,
                               const NIFFile::NIFVertex *v1)
{
  NIFFile::NIFVertex  v;
  int     x = roundFloat(v0->x);
  int     y = roundFloat(v0->y);
  if (float(std::fabs(v1->y - v0->y)) >= (1.0f / 1024.0f))
  {
    double  dy1 = 1.0 / (double(v1->y) - double(v0->y));
    int     y1 = roundFloat(v1->y);
    while (true)
    {
      double  w1 = (double(y) - double(v0->y)) * dy1;
      if (w1 > -0.000001 && w1 < 1.000001)
      {
        float   xf = (v1->x - v0->x) * float(w1) + v0->x;
        x = roundFloat(xf);
        if (float(std::fabs(xf - float(x))) < (1.0f / 1024.0f))
        {
          float   w0 = float(1.0 - w1);
          v.z = (v0->z * w0) + (v1->z * float(w1));
          drawPixel(x, y, 0.0f, 0.0f, v, *v0, *v1, *v0, w0, float(w1), 0.0f);
        }
      }
      if (y == y1)
        break;
      y = y + (y < y1 ? 1 : -1);
    }
  }
  else if (float(std::fabs(v1->x - v0->x)) >= (1.0f / 1024.0f))
  {
    double  dx1 = 1.0 / (double(v1->x) - double(v0->x));
    int     x1 = roundFloat(v1->x);
    while (true)
    {
      double  w1 = (double(x) - double(v0->x)) * dx1;
      if (w1 > -0.000001 && w1 < 1.000001)
      {
        float   yf = (v1->y - v0->y) * float(w1) + v0->y;
        y = roundFloat(yf);
        if (float(std::fabs(yf - float(y))) < (1.0f / 1024.0f))
        {
          float   w0 = float(1.0 - w1);
          v.z = (v0->z * w0) + (v1->z * float(w1));
          drawPixel(x, y, 0.0f, 0.0f, v, *v0, *v1, *v0, w0, float(w1), 0.0f);
        }
      }
      if (x == x1)
        break;
      x = x + (x < x1 ? 1 : -1);
    }
  }
  else if (float(std::fabs(v0->x - float(x))) < (1.0f / 1024.0f) &&
           float(std::fabs(v0->y - float(y))) < (1.0f / 1024.0f))
  {
    v.z = v0->z;
    drawPixel(x, y, 0.0f, 0.0f, v, *v0, *v0, *v0, 1.0f, 0.0f, 0.0f);
  }
}

inline void Plot3D_TriShape::calculateTangent(
    float& x, float& y, float& z, float v0, float v1, float v2,
    float x0, float y0, float z0, float x1, float y1, float z1,
    float x2, float y2, float z2, bool n)
{
  float   tmpX = ((v2 - v0) * (x1 - x0)) - ((v1 - v0) * (x2 - x0));
  float   tmpY = ((v2 - v0) * (y1 - y0)) - ((v1 - v0) * (y2 - y0));
  float   tmpZ = ((v2 - v0) * (z1 - z0)) - ((v1 - v0) * (z2 - z0));
  float   tmp = (tmpX * tmpX) + (tmpY * tmpY) + (tmpZ * tmpZ);
  if (BRANCH_EXPECT((tmp > 0.0f), true))
  {
    tmp = (!n ? 1.0f : -1.0f) / float(std::sqrt(tmp));
    x = tmpX * tmp;
    y = tmpY * tmp;
    z = tmpZ * tmp;
  }
}

void Plot3D_TriShape::drawTriangles()
{
  for (size_t n = 0; n < triangleBuf.size(); n++)
  {
    mipLevel = 15.0f;
    const NIFFile::NIFVertex  *v0, *v1, *v2;
    {
      const Triangle& t = triangleBuf[n];
      v0 = &(vertexBuf.front()) + triangleData[t.n].v0;
      v1 = &(vertexBuf.front()) + triangleData[t.n].v1;
      v2 = &(vertexBuf.front()) + triangleData[t.n].v2;
    }
    float   xyArea2 = float(((double(v1->x) - double(v0->x))
                             * (double(v2->y) - double(v0->y)))
                            - ((double(v2->x) - double(v0->x))
                               * (double(v1->y) - double(v0->y))));
    invNormals = (xyArea2 >= 0.0f);
    bitangentX = 1.0f;
    bitangentY = 0.0f;
    bitangentZ = 0.0f;
    tangentX = 0.0f;
    tangentY = 1.0f;
    tangentZ = 0.0f;
    xyArea2 = float(std::fabs(xyArea2));
    if (xyArea2 < (1.0f / 1048576.0f))  // if area < 2^-21 square pixels
    {
      drawLine(v0, v1);
      drawLine(v1, v2);
      drawLine(v2, v0);
      continue;
    }
    // sort vertices by Y coordinate
    if (v0->y > v1->y)
    {
      const NIFFile::NIFVertex  *tmp = v0;
      v0 = v1;
      v1 = tmp;
    }
    if (v1->y > v2->y)
    {
      const NIFFile::NIFVertex  *tmp;
      if (v0->y > v2->y)
      {
        tmp = v0;
        v0 = v2;
        v2 = tmp;
      }
      tmp = v1;
      v1 = v2;
      v2 = tmp;
    }
    double  x0 = double(v0->x);
    double  y0 = double(v0->y);
    double  x1 = double(v1->x);
    double  y1 = double(v1->y);
    double  x2 = double(v2->x);
    double  y2 = double(v2->y);
    double  r2xArea = 1.0 / (((x1 - x0) * (y2 - y0)) - ((x2 - x0) * (y1 - y0)));
    NIFFile::NIFVertex  v;
    float   txtU0 = v0->getU() * textureScaleU + textureOffsetU;
    float   txtV0 = v0->getV() * textureScaleV + textureOffsetV;
    float   txtU1 = v1->getU() * textureScaleU + textureOffsetU;
    float   txtV1 = v1->getV() * textureScaleV + textureOffsetV;
    float   txtU2 = v2->getU() * textureScaleU + textureOffsetU;
    float   txtV2 = v2->getV() * textureScaleV + textureOffsetV;
    if (BRANCH_EXPECT(textureD, true))
    {
      float   uvArea2 = ((txtU1 - txtU0) * (txtV2 - txtV0))
                        - ((txtU2 - txtU0) * (txtV1 - txtV0));
      if (BRANCH_EXPECT((textureN && uvArea2 != 0.0f), true))
      {
        calculateTangent(bitangentX, bitangentY, bitangentZ,
                         txtV0, txtV1, txtV2, v0->x, v0->y, v0->z,
                         v1->x, v1->y, v1->z, v2->x, v2->y, v2->z,
                         (uvArea2 < 0.0f));
        calculateTangent(tangentX, tangentY, tangentZ,
                         txtU0, txtU2, txtU1, v0->x, v0->y, v0->z,
                         v2->x, v2->y, v2->z, v1->x, v1->y, v1->z,
                         (uvArea2 < 0.0f));
      }
      uvArea2 = float(std::fabs(uvArea2));
      bool    integerMipLevel = true;
      if (xyArea2 > uvArea2)
      {
        float   txtW = float(textureD->getWidth());
        float   txtH = float(textureD->getHeight());
        uvArea2 *= (txtW * txtH);
        mipLevel = 0.0f;
        // calculate base 4 logarithm of texel area / pixel area
        if (uvArea2 > xyArea2)
        {
          mipLevel = float(std::log2(float(std::fabs(r2xArea * uvArea2))))
                     * 0.5f;
        }
        int     mipLevel_i = int(mipLevel);
        mipLevel = mipLevel - float(mipLevel_i);
        integerMipLevel = (mipLevel < 0.0625f || mipLevel >= 0.9375f);
        if (integerMipLevel)
        {
          mipLevel_i += int(mipLevel >= 0.5f);
          mipLevel = 0.0f;
        }
        float   txtScale = float(65536 >> mipLevel_i) * (1.0f / 65536.0f);
        txtW *= txtScale;
        txtH *= txtScale;
        txtU0 *= txtW;
        txtV0 *= txtH;
        txtU1 *= txtW;
        txtV1 *= txtH;
        txtU2 *= txtW;
        txtV2 *= txtH;
        mipLevel += float(mipLevel_i);
      }
    }
    double  dy2 = 1.0 / (y2 - y0);
    double  a1 = (y2 - y0) * r2xArea;
    double  b1 = -((x2 - x0) * a1);
    double  a2 = (y0 - y1) * r2xArea;
    double  b2 = 1.0 - ((x2 - x0) * a2);
    int     y = int(y0 + (y0 < 0.0 ? -0.0000005 : 0.9999995));
    int     yMax = int(y2 + (y2 < 0.0 ? -0.9999995 : 0.0000005));
    double  w1Step = (a1 < 0.0 ? -a1 : a1);
    double  w2Step = (a1 < 0.0 ? -a2 : a2);
    int     xStep = (a1 < 0.0 ? -1 : 1);
    do
    {
      double  yf = (double(y) - y0) * dy2;
      int     x = roundFloat(float((x2 - x0) * yf + x0));
      double  w1 = ((double(x) - x0) * a1) + (yf * b1);
      if (!(w1 >= -0.000001))
      {
        w1 += w1Step;
        x += xStep;
      }
      double  w2 = ((double(x) - x0) * a2) + (yf * b2);
      double  w0 = 1.0 - (w1 + w2);
      while (!(!(w0 >= -0.000001) | !(w2 >= -0.000001)))
      {
        v.z = (v0->z * float(w0)) + (v1->z * float(w1)) + (v2->z * float(w2));
        float   txtU, txtV;
        txtU = (txtU0 * float(w0)) + (txtU1 * float(w1)) + (txtU2 * float(w2));
        txtV = (txtV0 * float(w0)) + (txtV1 * float(w1)) + (txtV2 * float(w2));
        drawPixel(x, y, txtU, txtV,
                  v, *v0, *v1, *v2, float(w0), float(w1), float(w2));
        w1 += w1Step;
        w2 += w2Step;
        w0 = 1.0 - (w1 + w2);
        x += xStep;
      }
    }
    while (++y <= yMax);
  }
}

Plot3D_TriShape::Plot3D_TriShape(
    unsigned int *outBufRGBW, float *outBufZ, int imageWidth, int imageHeight,
    float vclrGamma)
  : bufRGBW(outBufRGBW),
    bufZ(outBufZ),
    width(imageWidth),
    height(imageHeight),
    textureD((DDSTexture *) 0),
    textureG((DDSTexture *) 0),
    textureN((DDSTexture *) 0),
    textureE((DDSTexture *) 0),
    textureS((DDSTexture *) 0),
    textureR((DDSTexture *) 0),
    textureScaleN(1.0f),
    textureScaleS(1.0f),
    textureScaleR(1.0f),
    mipLevel(15.0f),
    alphaThresholdScaled(0U),
    light_X(0.0f),
    light_Y(0.0f),
    light_Z(1.0f),
    reflectionLevel(0),
    envMapUVScale(0.25f / float(imageHeight)),
    invNormals(false),
    bitangentX(1.0f),
    bitangentY(0.0f),
    bitangentZ(0.0f),
    tangentX(0.0f),
    tangentY(1.0f),
    tangentZ(0.0f),
    drawPixelFunction(&drawPixel_Water),
    debugMode(0),
    viewTransformPtr((NIFFile::NIFVertexTransform *) 0)
{
  setLightingFunction(defaultLightingPolynomial);
  vclrTable.resize(256, 0);
  for (int i = 1; i < 256; i++)
  {
    float   tmp = float(std::pow(double(i) * (1.0 / 255.0), 1.0 / vclrGamma));
    vclrTable[i] = (unsigned short) roundFloat(tmp * 32768.0f);
  }
}

Plot3D_TriShape::~Plot3D_TriShape()
{
}

Plot3D_TriShape& Plot3D_TriShape::operator=(const NIFFile::NIFTriShape& t)
{
  vertexCnt = t.vertexCnt;
  triangleCnt = t.triangleCnt;
  vertexData = t.vertexData;
  triangleData = t.triangleData;
  vertexTransform = t.vertexTransform;
  flags = t.flags;
  gradientMapV = t.gradientMapV;
  envMapScale = t.envMapScale;
  alphaThreshold = t.alphaThreshold;
  specularColor = t.specularColor;
  specularScale = t.specularScale;
  specularSmoothness = t.specularSmoothness;
  texturePathCnt = t.texturePathCnt;
  texturePaths = t.texturePaths;
  materialPath = t.materialPath;
  textureOffsetU = t.textureOffsetU;
  textureOffsetV = t.textureOffsetV;
  textureScaleU = t.textureScaleU;
  textureScaleV = t.textureScaleV;
  name = t.name;
  return (*this);
}

void Plot3D_TriShape::setLightingFunction(const float *a)
{
  for (int i = 0; i < 6; i++)
    lightingPolynomial[i] = a[i];
  lightTable.resize(512);
  for (int i = 0; i < 512; i++)
  {
    float   x =
        float(i < 256 ? (i < 128 ? i : 128) : (i < 384 ? -128 : (i - 512)))
        * (1.0f / 128.0f);
    float   y =
        ((((x * a[5] + a[4]) * x + a[3]) * x + a[2]) * x + a[1]) * x + a[0];
    int     tmp = roundFloat(y * 65536.0f);
    lightTable[i] = (tmp > 0 ? (tmp < 0x0003FF00 ? tmp : 0x0003FF00) : 0);
  }
}

void Plot3D_TriShape::getLightingFunction(float *a) const
{
  for (int i = 0; i < 6; i++)
    a[i] = lightingPolynomial[i];
}

void Plot3D_TriShape::getDefaultLightingFunction(float *a)
{
  for (int i = 0; i < 6; i++)
    a[i] = defaultLightingPolynomial[i];
}

void Plot3D_TriShape::drawTriShape(
    const NIFFile::NIFVertexTransform& modelTransform,
    const NIFFile::NIFVertexTransform& viewTransform,
    float lightX, float lightY, float lightZ,
    const DDSTexture * const *textures, size_t textureCnt)
{
  bool    isWater = bool(flags & 0x02);
  if (!((textureCnt >= 1 && textures[0]) || isWater))
    return;
  viewTransformPtr = &viewTransform;
  size_t  triangleCntRendered =
      transformVertexData(modelTransform, viewTransform,
                          lightX, lightY, lightZ);
  if (!triangleCntRendered)
    return;
  textureD = (DDSTexture *) 0;
  textureG = (DDSTexture *) 0;
  textureN = (DDSTexture *) 0;
  textureE = (DDSTexture *) 0;
  textureS = (DDSTexture *) 0;
  textureR = (DDSTexture *) 0;
  textureScaleN = 1.0f;
  textureScaleS = 1.0f;
  textureScaleR = 1.0f;
  mipLevel = 15.0f;
  alphaThresholdScaled = (unsigned int) alphaThreshold << 24;
  light_X = lightX;
  light_Y = lightY;
  light_Z = lightZ;
  reflectionLevel = 0;
  if (textureCnt >= 1)
    textureD = textures[0];
  if (textureCnt >= 4)
    textureG = textures[3];
  if (textureCnt >= 2)
    textureN = textures[1];
  if (isWater)
  {
    if (!textureN)
      textureN = textureD;
    else
      textureD = textureN;
    drawPixelFunction = &drawPixel_Water;
  }
  else if (!(textureN && (textureD->getWidth() * textureN->getHeight())
                         == (textureD->getHeight() * textureN->getWidth())))
  {
    textureN = (DDSTexture *) 0;
    drawPixelFunction = &drawPixel_Diffuse;
  }
  else
  {
    textureScaleN = float(textureN->getWidth()) / float(textureD->getWidth());
    if (!(textureCnt >= 5 && textures[4]))
    {
      drawPixelFunction = &drawPixel_Normal;
    }
    else
    {
      textureE = textures[4];
      reflectionLevel =
          roundFloat(float(lightTable[128])
                     * (float(int(envMapScale)) * (0.7217095f / 32768.0f)));
      if (textureCnt >= 10 && textures[8] && textures[9])       // Fallout 76
      {
        textureR = textures[8];
        textureS = textures[9];
        textureScaleR =
            float(textureR->getWidth()) / float(textureD->getWidth());
        textureScaleS =
            float(textureS->getWidth()) / float(textureD->getWidth());
        drawPixelFunction = &drawPixel_NormalRefl;
      }
      else if (textureCnt >= 7 && textures[6])  // Fallout 4
      {
        textureS = textures[6];
        textureScaleS =
            float(textureS->getWidth()) / float(textureD->getWidth());
        drawPixelFunction = &drawPixel_NormalEnvS;
      }
      else if (textureCnt >= 6 && textures[5])  // Skyrim with environment mask
      {
        textureS = textures[5];
        textureScaleS =
            float(textureS->getWidth()) / float(textureD->getWidth());
        drawPixelFunction = &drawPixel_NormalEnvM;
      }
      else
      {
        drawPixelFunction = &drawPixel_NormalEnv;
      }
    }
  }
  if (BRANCH_EXPECT(debugMode, false))
    drawPixelFunction = &drawPixel_Debug;
  drawTriangles();
}

void Plot3D_TriShape::renderWater(
    const NIFFile::NIFVertexTransform& viewTransform,
    unsigned int waterColor, float lightX, float lightY, float lightZ,
    const DDSTexture *envMap, float envMapLevel)
{
  viewTransformPtr = &viewTransform;
  viewTransform.rotateXYZ(lightX, lightY, lightZ);
  textureE = envMap;
  envMapLevel = envMapLevel * (lightingPolynomial[0] * 256.0f);
  light_X = lightX;
  light_Y = lightY;
  light_Z = lightZ;
  unsigned int  *p = bufRGBW;
  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x++, p++)
    {
      unsigned int  c = *p;
      if (!((c + 0x01000000U) & 0xFE000000U))
        continue;
      float   normalX = float(int((c >> 16) & 0xFFU) - 128) * (1.0f / 126.0f);
      float   normalY = float(int((c >> 24) & 0xFFU) - 128) * (1.0f / 126.0f);
      float   normalZ = 0.0f;
      float   normalXY2 = (normalX * normalX) + (normalY * normalY);
      if (BRANCH_EXPECT((normalXY2 >= 1.0f), false))
      {
        // approximates 1.0 / sqrt(normalXY2)
        normalXY2 = (3.0f - normalXY2) * 0.5f;
        normalX = normalX * normalXY2;
        normalY = normalY * normalXY2;
        normalXY2 = 1.0f;
      }
      else
      {
        normalZ = -(float(std::sqrt(1.0001f - normalXY2)));
      }
      int     l = calculateLighting(normalX, normalY, normalZ);
      unsigned int  tmp = multiplyWithLight(waterColor, l);
      c = ((c & 0x001FU) << 3) | ((c & 0x07E0U) << 5) | ((c & 0xF800U) << 8);
      c = blendRGBA32(c, tmp, int(waterColor >> 24));
      if (envMap)
      {
        reflectionLevel = roundFloat(envMapLevel * (normalXY2 * 0.75f + 0.25f));
        c = addReflection(c, environmentMap(normalX, normalY, normalZ, x, y));
      }
      *p = c;
    }
  }
}


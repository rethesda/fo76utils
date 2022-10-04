
#include "common.hpp"
#include "nif_view.hpp"

#include <algorithm>

Renderer::Renderer(std::uint32_t *outBufRGBA, float *outBufZ,
                   int imageWidth, int imageHeight, unsigned int nifVersion)
  : lightX(0.0f),
    lightY(0.0f),
    lightZ(1.0f),
    waterEnvMapLevel(1.0f),
    waterColor(0xC0804000U),
    whiteTexture(0xFFFFFFFFU)
{
  threadCnt = int(std::thread::hardware_concurrency());
  if (threadCnt < 1 || imageHeight < 64)
    threadCnt = 1;
  else if (threadCnt > 8)
    threadCnt = 8;
  ba2File = (BA2File *) 0;
  renderers.resize(size_t(threadCnt), (Plot3D_TriShape *) 0);
  threadErrMsg.resize(size_t(threadCnt));
  viewOffsetY.resize(size_t(threadCnt + 1), 0);
  unsigned int  renderMode =
      (nifVersion < 0x80U ? 7U : (nifVersion < 0x90U ? 11U : 15U));
  try
  {
    for (size_t i = 0; i < renderers.size(); i++)
    {
      renderers[i] = new Plot3D_TriShape(outBufRGBA, outBufZ,
                                         imageWidth, imageHeight, renderMode);
    }
  }
  catch (...)
  {
    for (size_t i = 0; i < renderers.size(); i++)
    {
      if (renderers[i])
      {
        delete renderers[i];
        renderers[i] = (Plot3D_TriShape *) 0;
      }
    }
    throw;
  }
  if (outBufRGBA && outBufZ)
    setBuffers(outBufRGBA, outBufZ, imageWidth, imageHeight, 1.0f);
}

Renderer::~Renderer()
{
  for (size_t i = 0; i < renderers.size(); i++)
  {
    if (renderers[i])
      delete renderers[i];
  }
  for (std::map< std::string, DDSTexture * >::iterator
           i = textureSet.begin(); i != textureSet.end(); i++)
  {
    if (i->second)
      delete i->second;
  }
}

void Renderer::setBuffers(std::uint32_t *outBufRGBA, float *outBufZ,
                          int imageWidth, int imageHeight, float envMapScale)
{
  float   y0 = 0.0f;
  for (size_t i = 0; i < renderers.size(); i++)
  {
    int     y0i = roundFloat(y0);
    int     y1i = imageHeight;
    if ((i + 1) < renderers.size())
    {
      float   y1 = float(int(i + 1)) / float(int(renderers.size()));
      y1 = (y1 - 0.5f) * (y1 - 0.5f) * 2.0f;
      if (i < (renderers.size() >> 1))
        y1 = 0.5f - y1;
      else
        y1 = 0.5f + y1;
      y1 = y1 * float(imageHeight);
      y1i = roundFloat(y1);
      y0 = y1;
    }
    viewOffsetY[i] = y0i;
    size_t  offs = size_t(y0i) * size_t(imageWidth);
    renderers[i]->setBuffers(outBufRGBA + offs, outBufZ + offs,
                             imageWidth, y1i - y0i);
    renderers[i]->setEnvMapOffset(float(imageWidth) * -0.5f,
                                  float(imageHeight) * -0.5f + float(y0i),
                                  float(imageHeight) * envMapScale);
  }
  viewOffsetY[renderers.size()] = imageHeight;
}

const DDSTexture * Renderer::loadTexture(const std::string *texturePath)
{
  if (!texturePath || texturePath->empty() || !ba2File)
    return (DDSTexture *) 0;
  DDSTexture  *t = (DDSTexture *) 0;
  textureSetMutex.lock();
  try
  {
    std::map< std::string, DDSTexture * >::iterator i =
        textureSet.find(*texturePath);
    if (i == textureSet.end())
    {
      try
      {
        ba2File->extractFile(fileBuf, *texturePath);
        t = new DDSTexture(&(fileBuf.front()), fileBuf.size());
      }
      catch (FO76UtilsError& e)
      {
        std::fprintf(stderr, "Warning: failed to load texture '%s': %s\n",
                     texturePath->c_str(), e.what());
        t = (DDSTexture *) 0;
      }
      i = textureSet.insert(std::pair< std::string, DDSTexture * >(
                                *texturePath, t)).first;
    }
    t = i->second;
  }
  catch (...)
  {
    textureSetMutex.unlock();
    if (t)
      delete t;
    return (DDSTexture *) 0;
  }
  textureSetMutex.unlock();
  return t;
}

void Renderer::threadFunction(Renderer *p, size_t n)
{
  p->threadErrMsg[n].clear();
  try
  {
    std::vector< TriShapeSortObject > sortBuf;
    sortBuf.reserve(p->meshData.size());
    NIFFile::NIFVertexTransform vt(p->viewTransform);
    NIFFile::NIFVertexTransform mt(p->modelTransform);
    mt *= vt;
    vt.offsY = vt.offsY - float(p->viewOffsetY[n]);
    for (size_t i = 0; i < p->meshData.size(); i++)
    {
      if (p->meshData[i].m.flags & BGSMFile::Flag_TSHidden)
        continue;
      NIFFile::NIFBounds  b;
      p->meshData[i].calculateBounds(b, &mt);
      if (roundFloat(b.xMax()) < 0 ||
          roundFloat(b.yMin()) > p->viewOffsetY[n + 1] ||
          roundFloat(b.yMax()) < p->viewOffsetY[n] ||
          b.zMax() < 0.0f)
      {
        continue;
      }
      TriShapeSortObject  tmp;
      tmp.ts = &(p->meshData.front()) + i;
      tmp.z = double(b.zMin());
      if (tmp.ts->m.flags & BGSMFile::Flag_TSAlphaBlending)
        tmp.z = double(0x02000000) - tmp.z;
      sortBuf.push_back(tmp);
    }
    if (sortBuf.size() < 1)
      return;
    std::stable_sort(sortBuf.begin(), sortBuf.end());
    for (size_t i = 0; i < sortBuf.size(); i++)
    {
      const NIFFile::NIFTriShape& ts = *(sortBuf[i].ts);
      *(p->renderers[n]) = ts;
      const DDSTexture  *textures[10];
      unsigned int  textureMask = 0U;
      if (BRANCH_UNLIKELY(ts.m.flags & BGSMFile::Flag_TSWater))
      {
        if (bool(textures[1] = p->loadTexture(&(p->waterTexture))))
          textureMask |= 0x0002U;
        if (bool(textures[4] = p->loadTexture(&(p->defaultEnvMap))))
          textureMask |= 0x0010U;
        p->renderers[n]->m.envMapScale =
            floatToUInt8Clamped(p->waterEnvMapLevel, 128.0f);
        p->renderers[n]->m.emissiveColor = p->waterColor;
      }
      else
      {
        unsigned int  texturePathMask =
            (!(ts.m.flags & BGSMFile::Flag_Glow) ? 0x037BU : 0x037FU)
            & (unsigned int) ts.m.texturePathMask;
        for (size_t j = 0; j < 10; j++, texturePathMask >>= 1)
        {
          if (texturePathMask & 1)
          {
            if (bool(textures[j] = p->loadTexture(ts.texturePaths[j])))
              textureMask |= (1U << (unsigned char) j);
          }
        }
        if (!(textureMask & 0x0001U))
        {
          textures[0] = &(p->whiteTexture);
          textureMask |= 0x0001U;
        }
        if (!(textureMask & 0x0010U) && ts.m.envMapScale > 0)
        {
          if (bool(textures[4] = p->loadTexture(&(p->defaultEnvMap))))
            textureMask |= 0x0010U;
        }
      }
      p->renderers[n]->drawTriShape(p->modelTransform, vt,
                                    p->lightX, p->lightY, p->lightZ,
                                    textures, textureMask);
    }
  }
  catch (std::exception& e)
  {
    p->threadErrMsg[n] = e.what();
    if (p->threadErrMsg[n].empty())
      p->threadErrMsg[n] = "unknown error in render thread";
  }
}

void Renderer::renderModel()
{
  std::vector< std::thread * >  threads(renderers.size(), (std::thread *) 0);
  try
  {
    for (size_t i = 0; i < threads.size(); i++)
      threads[i] = new std::thread(threadFunction, this, i);
    for (size_t i = 0; i < threads.size(); i++)
    {
      if (threads[i])
      {
        threads[i]->join();
        delete threads[i];
        threads[i] = (std::thread *) 0;
      }
      if (!threadErrMsg[i].empty())
        throw FO76UtilsError(1, threadErrMsg[i].c_str());
    }
  }
  catch (...)
  {
    for (size_t i = 0; i < threads.size(); i++)
    {
      if (threads[i])
      {
        threads[i]->join();
        delete threads[i];
      }
    }
    throw;
  }
}

static const char *cubeMapPaths[24] =
{
  "textures/cubemaps/bleakfallscube_e.dds",                     // Skyrim
  "textures/shared/cubemaps/mipblur_defaultoutside1.dds",       // Fallout 4
  "textures/shared/cubemaps/mipblur_defaultoutside1.dds",       // Fallout 76
  "textures/cubemaps/wrtemple_e.dds",
  "textures/shared/cubemaps/outsideoldtownreflectcube_e.dds",
  "textures/shared/cubemaps/outsideoldtownreflectcube_e.dds",
  "textures/cubemaps/duncaveruingreen_e.dds",
  "textures/shared/cubemaps/cgprewarstreet_e.dds",
  "textures/shared/cubemaps/swampcube.dds",
  "textures/cubemaps/chrome_e.dds",
  "textures/shared/cubemaps/metalchrome01cube_e.dds",
  "textures/shared/cubemaps/metalchrome01cube_e.dds",
  "textures/cubemaps/cavegreencube_e.dds",
  "textures/shared/cubemaps/outsideday01.dds",
  "textures/shared/cubemaps/outsideday01.dds",
  "textures/cubemaps/mghallcube_e.dds",
  "textures/shared/cubemaps/cgplayerhousecube.dds",
  "textures/shared/cubemaps/chrome_e.dds",
  "textures/cubemaps/caveicecubemap_e.dds",
  "textures/shared/cubemaps/inssynthproductionpoolcube.dds",
  "textures/shared/cubemaps/vault111cryocube.dds",
  "textures/cubemaps/minecube_e.dds",
  "textures/shared/cubemaps/memorydencube.dds",
  "textures/shared/cubemaps/mipblur_defaultoutside_pitt.dds"
};

static float viewRotations[27] =
{
  54.73561f,  180.0f,     45.0f,        // isometric from NW
  54.73561f,  180.0f,     135.0f,       // isometric from SW
  54.73561f,  180.0f,     -135.0f,      // isometric from SE
  54.73561f,  180.0f,     -45.0f,       // isometric from NE
  180.0f,     0.0f,       0.0f,         // top
  -90.0f,     0.0f,       0.0f,         // front
  -90.0f,     0.0f,       90.0f,        // right
  -90.0f,     0.0f,       180.0f,       // back
  -90.0f,     0.0f,       -90.0f        // left
};

static inline float degreesToRadians(float x)
{
  return float(double(x) * (std::atan(1.0) / 45.0));
}

void Renderer::renderMeshToFile(const char *outFileName,
                                const NIFFile& nifFile, const BA2File& ba2File,
                                int imageWidth, int imageHeight)
{
  float   modelRotationX = 0.0f;
  float   modelRotationY = 0.0f;
  float   modelRotationZ = 0.0f;
  float   lightRotationY = 56.25f;
  float   lightRotationZ = -135.0f;
  float   lightLevel = 1.0f;
  int     viewRotation = 0;     // isometric from NW
  float   viewScale = 1.0f;
  int     envMapNum = 0;
  float   envMapScale = 1.0f;
  unsigned int  debugMode = 0;

  size_t  imageDataSize = size_t(imageWidth) * size_t(imageHeight);
  std::vector< std::uint32_t >  outBufRGBA(imageDataSize, 0U);
  {
    std::vector< float >  outBufZ(imageDataSize, 16777216.0f);
    Renderer  renderer(&(outBufRGBA.front()), &(outBufZ.front()),
                       imageWidth, imageHeight, nifFile.getVersion());
    renderer.ba2File = &ba2File;
    nifFile.getMesh(renderer.meshData);
    renderer.waterTexture = "textures/water/defaultwater.dds";
    renderer.modelTransform =
        NIFFile::NIFVertexTransform(
            1.0f, degreesToRadians(modelRotationX),
            degreesToRadians(modelRotationY),
            degreesToRadians(modelRotationZ), 0.0f, 0.0f, 0.0f);
    renderer.viewTransform =
        NIFFile::NIFVertexTransform(
            1.0f,
            degreesToRadians(viewRotations[viewRotation * 3]),
            degreesToRadians(viewRotations[viewRotation * 3 + 1]),
            degreesToRadians(viewRotations[viewRotation * 3 + 2]),
            0.0f, 0.0f, 0.0f);
    NIFFile::NIFVertexTransform
        lightTransform(1.0f, 0.0f, degreesToRadians(lightRotationY),
                       degreesToRadians(lightRotationZ), 0.0f, 0.0f, 0.0f);
    renderer.lightX = lightTransform.rotateZX;
    renderer.lightY = lightTransform.rotateZY;
    renderer.lightZ = lightTransform.rotateZZ;
    renderer.waterEnvMapLevel = 1.0f;
    renderer.waterColor = 0xC0804000U;
    {
      NIFFile::NIFVertexTransform t(renderer.modelTransform);
      t *= renderer.viewTransform;
      NIFFile::NIFBounds  b;
      for (size_t i = 0; i < renderer.meshData.size(); i++)
      {
        // ignore if hidden
        if (!(renderer.meshData[i].m.flags & BGSMFile::Flag_TSHidden))
          renderer.meshData[i].calculateBounds(b, &t);
      }
      float   xScale = float(imageWidth) * 0.96875f;
      if (b.xMax() > b.xMin())
        xScale = xScale / (b.xMax() - b.xMin());
      float   yScale = float(imageHeight) * 0.96875f;
      if (b.yMax() > b.yMin())
        yScale = yScale / (b.yMax() - b.yMin());
      float   scale = (xScale < yScale ? xScale : yScale) * viewScale;
      renderer.viewTransform.scale = scale;
      renderer.viewTransform.offsX =
          0.5f * (float(imageWidth) - ((b.xMin() + b.xMax()) * scale));
      renderer.viewTransform.offsY =
          0.5f * (float(imageHeight) - ((b.yMin() + b.yMax()) * scale));
      renderer.viewTransform.offsZ = 1.0f - (b.zMin() * scale);
    }
    renderer.defaultEnvMap =
        std::string(cubeMapPaths[envMapNum * 3
                                 + int(nifFile.getVersion() >= 0x80)
                                 + int(nifFile.getVersion() >= 0x90)]);
    renderer.setBuffers(&(outBufRGBA.front()), &(outBufZ.front()),
                        imageWidth, imageHeight, envMapScale);
    {
      FloatVector4  a(renderer.renderers[0]->cubeMapToAmbient(
                          renderer.loadTexture(&(renderer.defaultEnvMap))));
      for (size_t i = 0; i < renderer.renderers.size(); i++)
      {
        renderer.renderers[i]->setLighting(
            FloatVector4(1.0f), a, FloatVector4(1.0f), lightLevel);
        renderer.renderers[i]->setDebugMode(debugMode, 0);
      }
    }
    renderer.renderModel();
  }

  int     w = (imageWidth + 1) >> 1;
  int     h = (imageHeight + 1) >> 1;
  imageDataSize = size_t(w) * size_t(h);
  std::vector< std::uint32_t >  downsampleBuf(imageDataSize);
  downsample2xFilter(&(downsampleBuf.front()), &(outBufRGBA.front()),
                     imageWidth, imageHeight, w);
#if USE_PIXELFMT_RGB10A2
  DDSOutputFile outFile(outFileName, w, h,
                        DDSInputFile::pixelFormatA2R10G10B10);
  outFile.writeImageData(&(downsampleBuf.front()), imageDataSize,
                         DDSInputFile::pixelFormatA2R10G10B10);
#else
  DDSOutputFile outFile(outFileName, w, h, DDSInputFile::pixelFormatRGB24);
  outFile.writeImageData(&(downsampleBuf.front()), imageDataSize,
                         DDSInputFile::pixelFormatRGB24);
#endif
}

static void updateRotation(float& rx, float& ry, float& rz,
                           int dx, int dy, int dz,
                           std::string& messageBuf, const char *msg)
{
  rx += (float(dx) * 2.8125f);
  ry += (float(dy) * 2.8125f);
  rz += (float(dz) * 2.8125f);
  rx = (rx < -180.0f ? (rx + 360.0f) : (rx > 180.0f ? (rx - 360.0f) : rx));
  ry = (ry < -180.0f ? (ry + 360.0f) : (ry > 180.0f ? (ry - 360.0f) : ry));
  rz = (rz < -180.0f ? (rz + 360.0f) : (rz > 180.0f ? (rz - 360.0f) : rz));
  if (!msg)
    msg = "";
  char    buf[64];
  std::snprintf(buf, 64, "%s %7.2f %7.2f %7.2f\n", msg, rx, ry, rz);
  buf[63] = '\0';
  messageBuf = buf;
}

static void updateLightColor(FloatVector4& lightColor, int dR, int dG, int dB,
                             std::string& messageBuf)
{
  int     r = roundFloat(float(std::log2(lightColor[0])) * 16.0f);
  lightColor[0] = float(std::exp2(float(r + dR) * 0.0625f));
  int     g = roundFloat(float(std::log2(lightColor[1])) * 16.0f);
  lightColor[1] = float(std::exp2(float(g + dG) * 0.0625f));
  int     b = roundFloat(float(std::log2(lightColor[2])) * 16.0f);
  lightColor[2] = float(std::exp2(float(b + dB) * 0.0625f));
  lightColor.maxValues(FloatVector4(0.0625f));
  lightColor.minValues(FloatVector4(4.0f));
  char    buf[64];
  std::snprintf(buf, 64,
                "Light color (linear color space): %7.4f %7.4f %7.4f\n",
                lightColor[0], lightColor[1], lightColor[2]);
  buf[63] = '\0';
  messageBuf = buf;
}

static void updateValueLogScale(float& s, int d, float minVal, float maxVal,
                                std::string& messageBuf, const char *msg)
{
  int     tmp = roundFloat(float(std::log2(s)) * 16.0f);
  s = float(std::exp2(float(tmp + d) * 0.0625f));
  s = (s > minVal ? (s < maxVal ? s : maxVal) : minVal);
  if (!msg)
    msg = "";
  char    buf[64];
  std::snprintf(buf, 64, "%s: %7.4f\n", msg, s);
  buf[63] = '\0';
  messageBuf = buf;
}

static void saveScreenshot(SDLDisplay& display, const std::string& nifFileName)
{
  size_t  n1 = nifFileName.rfind('/');
  n1 = (n1 != std::string::npos ? (n1 + 1) : 0);
  size_t  n2 = nifFileName.rfind('.');
  n2 = (n2 != std::string::npos ? n2 : 0);
  std::string fileName;
  if (n2 > n1)
    fileName.assign(nifFileName, n1, n2 - n1);
  else
    fileName = "nif_info";
  std::time_t t = std::time((std::time_t *) 0);
  {
    unsigned int  s = (unsigned int) (t % (std::time_t) (24 * 60 * 60));
    unsigned int  m = s / 60U;
    s = s % 60U;
    unsigned int  h = m / 60U;
    m = m % 60U;
    h = h % 24U;
    char    buf[16];
    std::sprintf(buf, "_%02u%02u%02u.dds", h, m, s);
    fileName += buf;
  }
  {
    display.blitSurface();
    const std::uint32_t *p = display.lockScreenSurface();
    int     w = display.getWidth() >> int(display.getIsDownsampled());
    int     h = display.getHeight() >> int(display.getIsDownsampled());
    DDSOutputFile f(fileName.c_str(), w, h,
#if USE_PIXELFMT_RGB10A2
                    DDSInputFile::pixelFormatA2R10G10B10
#else
                    DDSInputFile::pixelFormatRGB24
#endif
                    );
    size_t  pitch = display.getPitch();
    for (int y = 0; y < h; y++, p = p + pitch)
    {
#if USE_PIXELFMT_RGB10A2
      f.writeImageData(p, size_t(w), DDSInputFile::pixelFormatA2R10G10B10);
#else
      f.writeImageData(p, size_t(w), DDSInputFile::pixelFormatRGB24);
#endif
    }
    display.unlockScreenSurface();
  }
  display.printString("Saved screenshot to ");
  display.printString(fileName.c_str());
  display.printString("\n");
}

static const char *keyboardUsageString =
    "  \033[4m\033[38;5;228m0\033[m "
    "to \033[4m\033[38;5;228m5\033[m                "
    "Set debug render mode.                                          \n"
    "  \033[4m\033[38;5;228m+\033[m, "
    "\033[4m\033[38;5;228m-\033[m                  "
    "Zoom in or out.                                                 \n"
    "  \033[4m\033[38;5;228mKeypad 1, 3, 9, 7\033[m     "
    "Set isometric view from the SW, SE, NE, or NW (default).        \n"
    "  \033[4m\033[38;5;228mKeypad 2, 6, 8, 4, 5\033[m  "
    "Set view from the S, E, N, W, or top.                           \n"
    "  \033[4m\033[38;5;228mF1\033[m "
    "to \033[4m\033[38;5;228mF8\033[m              "
    "Select default cube map.                                        \n"
    "  \033[4m\033[38;5;228mA\033[m, "
    "\033[4m\033[38;5;228mD\033[m                  "
    "Rotate model around the Z axis.                                 \n"
    "  \033[4m\033[38;5;228mS\033[m, "
    "\033[4m\033[38;5;228mW\033[m                  "
    "Rotate model around the X axis.                                 \n"
    "  \033[4m\033[38;5;228mQ\033[m, "
    "\033[4m\033[38;5;228mE\033[m                  "
    "Rotate model around the Y axis.                                 \n"
    "  \033[4m\033[38;5;228mK\033[m, "
    "\033[4m\033[38;5;228mL\033[m                  "
    "Decrease or increase overall brightness.                        \n"
    "  \033[4m\033[38;5;228mU\033[m, "
    "\033[4m\033[38;5;228m7\033[m                  "
    "Decrease or increase light source red level.                    \n"
    "  \033[4m\033[38;5;228mI\033[m, "
    "\033[4m\033[38;5;228m8\033[m                  "
    "Decrease or increase light source green level.                  \n"
    "  \033[4m\033[38;5;228mO\033[m, "
    "\033[4m\033[38;5;228m9\033[m                  "
    "Decrease or increase light source blue level.                   \n"
    "  \033[4m\033[38;5;228mLeft\033[m, "
    "\033[4m\033[38;5;228mRight\033[m           "
    "Rotate light vector around the Z axis.                          \n"
    "  \033[4m\033[38;5;228mUp\033[m, "
    "\033[4m\033[38;5;228mDown\033[m              "
    "Rotate light vector around the Y axis.                          \n"
    "  \033[4m\033[38;5;228mInsert\033[m, "
    "\033[4m\033[38;5;228mDelete\033[m        "
    "Zoom reflected environment in or out.                           \n"
    "  \033[4m\033[38;5;228mCaps Lock\033[m             "
    "Toggle fine adjustment of view and lighting parameters.         \n"
    "  \033[4m\033[38;5;228mPage Up\033[m               "
    "Enable downsampling (slow).                                     \n"
    "  \033[4m\033[38;5;228mPage Down\033[m             "
    "Disable downsampling.                                           \n"
    "  \033[4m\033[38;5;228mSpace\033[m, "
    "\033[4m\033[38;5;228mBackspace\033[m      "
    "Load next or previous file matching the pattern.                \n"
    "  \033[4m\033[38;5;228mF12\033[m "
    "or \033[4m\033[38;5;228mPrint Screen\033[m   "
    "Save screenshot.                                                \n"
    "  \033[4m\033[38;5;228mP\033[m                     "
    "Print current settings and file list.                           \n"
    "  \033[4m\033[38;5;228mH\033[m                     "
    "Show help screen.                                               \n"
    "  \033[4m\033[38;5;228mC\033[m                     "
    "Clear messages.                                                 \n"
    "  \033[4m\033[38;5;228mEsc\033[m                   "
    "Quit viewer.                                                    \n";

void Renderer::viewMeshes(SDLDisplay& display, const BA2File& ba2File,
                          const std::vector< std::string >& nifFileNames)
{
  if (nifFileNames.size() < 1)
    return;
  std::vector< SDLDisplay::SDLEvent > eventBuf;
  std::string messageBuf;
  bool    quitFlag = false;
  try
  {
    int     imageWidth = display.getWidth();
    int     imageHeight = display.getHeight();
    float   modelRotationX = 0.0f;
    float   modelRotationY = 0.0f;
    float   modelRotationZ = 0.0f;
    float   lightRotationX = 0.0f;
    float   lightRotationY = 56.25f;
    float   lightRotationZ = -135.0f;
    FloatVector4  lightColor(1.0f);
    float   lightLevel = 1.0f;
    int     viewRotation = 0;   // isometric from NW
    float   viewScale = 1.0f;
    int     envMapNum = 0;
    float   envMapScale = 1.0f;
    unsigned int  debugMode = 0;
    int     fileNum = 0;
    int     d = 4;              // scale of adjusting parameters

    size_t  imageDataSize = size_t(imageWidth) * size_t(imageHeight);
    std::vector< float >  outBufZ(imageDataSize);
    std::vector< unsigned char >  fileBuf;
    while (!quitFlag)
    {
      messageBuf += nifFileNames[fileNum];
      messageBuf += '\n';
      ba2File.extractFile(fileBuf, nifFileNames[fileNum]);
      NIFFile   nifFile(&(fileBuf.front()), fileBuf.size(), &ba2File);
      Renderer  renderer((std::uint32_t *) 0, (float *) 0,
                         imageWidth, imageHeight, nifFile.getVersion());
      renderer.ba2File = &ba2File;
      nifFile.getMesh(renderer.meshData);
      renderer.waterTexture = "textures/water/defaultwater.dds";

      bool    nextFileFlag = false;
      bool    screenshotFlag = false;
      unsigned char redrawFlags = 3;    // bit 0: blit only, bit 1: render
      while (!(nextFileFlag || quitFlag))
      {
        if (!messageBuf.empty())
        {
          display.printString(messageBuf.c_str());
          messageBuf.clear();
        }
        if (redrawFlags & 2)
        {
          renderer.modelTransform =
              NIFFile::NIFVertexTransform(
                  1.0f, degreesToRadians(modelRotationX),
                  degreesToRadians(modelRotationY),
                  degreesToRadians(modelRotationZ), 0.0f, 0.0f, 0.0f);
          renderer.viewTransform =
              NIFFile::NIFVertexTransform(
                  1.0f,
                  degreesToRadians(viewRotations[viewRotation * 3]),
                  degreesToRadians(viewRotations[viewRotation * 3 + 1]),
                  degreesToRadians(viewRotations[viewRotation * 3 + 2]),
                  0.0f, 0.0f, 0.0f);
          NIFFile::NIFVertexTransform
              lightTransform(
                  1.0f, degreesToRadians(lightRotationX),
                  degreesToRadians(lightRotationY),
                  degreesToRadians(lightRotationZ), 0.0f, 0.0f, 0.0f);
          renderer.lightX = lightTransform.rotateZX;
          renderer.lightY = lightTransform.rotateZY;
          renderer.lightZ = lightTransform.rotateZZ;
          renderer.waterEnvMapLevel = 1.0f;
          renderer.waterColor = 0xC0804000U;
          NIFFile::NIFVertexTransform t(renderer.modelTransform);
          t *= renderer.viewTransform;
          NIFFile::NIFBounds  b;
          for (size_t i = 0; i < renderer.meshData.size(); i++)
          {
            // ignore if hidden
            if (!(renderer.meshData[i].m.flags & BGSMFile::Flag_TSHidden))
              renderer.meshData[i].calculateBounds(b, &t);
          }
          float   xScale = float(imageWidth) * 0.96875f;
          if (b.xMax() > b.xMin())
            xScale = xScale / (b.xMax() - b.xMin());
          float   yScale = float(imageHeight) * 0.96875f;
          if (b.yMax() > b.yMin())
            yScale = yScale / (b.yMax() - b.yMin());
          float   scale = (xScale < yScale ? xScale : yScale) * viewScale;
          renderer.viewTransform.scale = scale;
          renderer.viewTransform.offsX =
              0.5f * (float(imageWidth) - ((b.xMin() + b.xMax()) * scale));
          renderer.viewTransform.offsY =
              0.5f * (float(imageHeight) - ((b.yMin() + b.yMax()) * scale));
          renderer.viewTransform.offsZ = 1.0f - (b.zMin() * scale);
          renderer.defaultEnvMap =
              std::string(cubeMapPaths[envMapNum * 3
                                       + int(nifFile.getVersion() >= 0x80)
                                       + int(nifFile.getVersion() >= 0x90)]);
          display.clearSurface();
          for (size_t i = 0; i < imageDataSize; i++)
            outBufZ[i] = 16777216.0f;
          std::uint32_t *outBufRGBA = display.lockDrawSurface();
          renderer.setBuffers(outBufRGBA, &(outBufZ.front()),
                              imageWidth, imageHeight, envMapScale);
          FloatVector4  a(renderer.renderers[0]->cubeMapToAmbient(
                              renderer.loadTexture(&(renderer.defaultEnvMap))));
          for (size_t i = 0; i < renderer.renderers.size(); i++)
          {
            renderer.renderers[i]->setLighting(
                lightColor, a, FloatVector4(1.0f), lightLevel);
            renderer.renderers[i]->setDebugMode(debugMode, 0);
          }
          renderer.renderModel();
          display.unlockDrawSurface();
          if (screenshotFlag)
          {
            saveScreenshot(display, nifFileNames[fileNum]);
            screenshotFlag = false;
          }
          display.drawText(0, -1, display.getTextRows(), 0.75f, 1.0f);
          redrawFlags = 1;
        }
        display.blitSurface();
        redrawFlags = 0;

        while (!(redrawFlags || nextFileFlag || quitFlag))
        {
          display.pollEvents(eventBuf, 10, false, false);
          for (size_t i = 0; i < eventBuf.size(); i++)
          {
            int     t = eventBuf[i].type();
            int     d1 = eventBuf[i].data1();
            if (t == SDLDisplay::SDLEventWindow)
            {
              if (d1 == 0)
                quitFlag = true;
              else if (d1 == 1)
                redrawFlags = 1;
              continue;
            }
            if (!(t == SDLDisplay::SDLEventKeyRepeat ||
                  t == SDLDisplay::SDLEventKeyDown))
            {
              continue;
            }
            redrawFlags = 2;
            switch (d1)
            {
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
                debugMode = (unsigned int) (d1 != '1' ? (d1 - '0') : 0);
                messageBuf += "Debug mode set to ";
                messageBuf += char(debugMode | 0x30U);
                messageBuf += '\n';
                break;
              case '-':
              case SDLDisplay::SDLKeySymKPMinus:
                updateValueLogScale(viewScale, -d, 0.0625f, 16.0f, messageBuf,
                                    "View scale");
                break;
              case '=':
              case SDLDisplay::SDLKeySymKPPlus:
                updateValueLogScale(viewScale, d, 0.0625f, 16.0f, messageBuf,
                                    "View scale");
                break;
              case SDLDisplay::SDLKeySymKP1 + 6:
                viewRotation = 0;
                messageBuf += "Isometric view from the NW\n";
                break;
              case SDLDisplay::SDLKeySymKP1:
                viewRotation = 1;
                messageBuf += "Isometric view from the SW\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 2:
                viewRotation = 2;
                messageBuf += "Isometric view from the SE\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 8:
                viewRotation = 3;
                messageBuf += "Isometric view from the NE\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 4:
                viewRotation = 4;
                messageBuf += "Top view\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 1:
                viewRotation = 5;
                messageBuf += "S view\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 5:
                viewRotation = 6;
                messageBuf += "E view\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 7:
                viewRotation = 7;
                messageBuf += "N view\n";
                break;
              case SDLDisplay::SDLKeySymKP1 + 3:
                viewRotation = 8;
                messageBuf += "W view\n";
                break;
              case SDLDisplay::SDLKeySymF1:
              case SDLDisplay::SDLKeySymF1 + 1:
              case SDLDisplay::SDLKeySymF1 + 2:
              case SDLDisplay::SDLKeySymF1 + 3:
              case SDLDisplay::SDLKeySymF1 + 4:
              case SDLDisplay::SDLKeySymF1 + 5:
              case SDLDisplay::SDLKeySymF1 + 6:
              case SDLDisplay::SDLKeySymF1 + 7:
                envMapNum = d1 - SDLDisplay::SDLKeySymF1;
                messageBuf += "Default environment map: ";
                messageBuf += cubeMapPaths[envMapNum * 3
                                           + int(nifFile.getVersion() >= 0x80)
                                           + int(nifFile.getVersion() >= 0x90)];
                messageBuf += '\n';
                break;
              case 'a':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, d, messageBuf, "Model rotation");
                break;
              case 'd':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, -d, messageBuf, "Model rotation");
                break;
              case 's':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               d, 0, 0, messageBuf, "Model rotation");
                break;
              case 'w':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               -d, 0, 0, messageBuf, "Model rotation");
                break;
              case 'q':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, -d, 0, messageBuf, "Model rotation");
                break;
              case 'e':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, d, 0, messageBuf, "Model rotation");
                break;
              case 'k':
                updateValueLogScale(lightLevel, -d, 0.0625f, 16.0f, messageBuf,
                                    "Brightness (linear color space)");
                break;
              case 'l':
                updateValueLogScale(lightLevel, d, 0.0625f, 16.0f, messageBuf,
                                    "Brightness (linear color space)");
                break;
              case SDLDisplay::SDLKeySymLeft:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, d, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymRight:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, -d, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymDown:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, d, 0, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymUp:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, -d, 0, messageBuf, "Light rotation");
                break;
              case '7':
                updateLightColor(lightColor, d, 0, 0, messageBuf);
                break;
              case 'u':
                updateLightColor(lightColor, -d, 0, 0, messageBuf);
                break;
              case '8':
                updateLightColor(lightColor, 0, d, 0, messageBuf);
                break;
              case 'i':
                updateLightColor(lightColor, 0, -d, 0, messageBuf);
                break;
              case '9':
                updateLightColor(lightColor, 0, 0, d, messageBuf);
                break;
              case 'o':
                updateLightColor(lightColor, 0, 0, -d, messageBuf);
                break;
              case SDLDisplay::SDLKeySymInsert:
                updateValueLogScale(envMapScale, d, 0.25f, 8.0f, messageBuf,
                                    "Reflection f scale");
                break;
              case SDLDisplay::SDLKeySymDelete:
                updateValueLogScale(envMapScale, -d, 0.25f, 8.0f, messageBuf,
                                    "Reflection f scale");
                break;
              case SDLDisplay::SDLKeySymCapsLock:
                d = (d == 1 ? 4 : 1);
                if (d == 1)
                  messageBuf += "Step size: 2.8125\302\260, exp2(1/16)\n";
                else
                  messageBuf += "Step size: 11.25\302\260, exp2(1/4)\n";
                break;
              case SDLDisplay::SDLKeySymPageUp:
              case SDLDisplay::SDLKeySymPageDown:
                if ((d1 == SDLDisplay::SDLKeySymPageUp)
                    == display.getIsDownsampled())
                {
                  redrawFlags = 0;
                  continue;
                }
                display.setEnableDownsample(d1 == SDLDisplay::SDLKeySymPageUp);
                imageWidth = display.getWidth();
                imageHeight = display.getHeight();
                imageDataSize = size_t(imageWidth) * size_t(imageHeight);
                outBufZ.resize(imageDataSize);
                if (display.getIsDownsampled())
                  messageBuf += "Downsampling enabled\n";
                else
                  messageBuf += "Downsampling disabled\n";
                break;
              case SDLDisplay::SDLKeySymBackspace:
                fileNum = (fileNum > 0 ? fileNum : int(nifFileNames.size()));
                fileNum--;
                nextFileFlag = true;
                break;
              case ' ':
                fileNum++;
                fileNum = (size_t(fileNum) < nifFileNames.size() ? fileNum : 0);
                nextFileFlag = true;
                break;
              case SDLDisplay::SDLKeySymF1 + 11:
              case SDLDisplay::SDLKeySymPrintScr:
                screenshotFlag = true;
                break;
              case 'p':
                display.clearTextBuffer();
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, 0, messageBuf, "Model rotation");
                display.printString(messageBuf.c_str());
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, 0, messageBuf, "Light rotation");
                display.printString(messageBuf.c_str());
                updateValueLogScale(lightLevel, 0, 0.25f, 4.0f, messageBuf,
                                    "Brightness (linear color space)");
                display.printString(messageBuf.c_str());
                updateLightColor(lightColor, 0, 0, 0, messageBuf);
                display.printString(messageBuf.c_str());
                updateValueLogScale(viewScale, 0, 0.0625f, 16.0f, messageBuf,
                                    "View scale");
                display.printString(messageBuf.c_str());
                updateValueLogScale(envMapScale, 0, 0.5f, 4.0f, messageBuf,
                                    "Reflection f scale");
                if (d == 1)
                  messageBuf += "Step size: 2.8125\302\260, exp2(1/16)\n";
                else
                  messageBuf += "Step size: 11.25\302\260, exp2(1/4)\n";
                if (display.getIsDownsampled())
                  messageBuf += "Downsampling enabled\n";
                else
                  messageBuf += "Downsampling disabled\n";
                messageBuf += "Default environment map: ";
                messageBuf += cubeMapPaths[envMapNum * 3
                                           + int(nifFile.getVersion() >= 0x80)
                                           + int(nifFile.getVersion() >= 0x90)];
                messageBuf += "\nFile list:\n";
                {
                  int     n0 = 0;
                  int     n1 = int(nifFileNames.size());
                  while ((n1 - n0) > (display.getTextRows() - 12))
                  {
                    if (fileNum < ((n0 + n1) >> 1))
                      n1--;
                    else
                      n0++;
                  }
                  for ( ; n0 < n1; n0++)
                  {
                    messageBuf += (n0 != fileNum ?
                                   "  " : "  \033[44m\033[37m\033[1m");
                    messageBuf += nifFileNames[n0];
                    messageBuf += "\033[m  \n";
                  }
                }
                continue;
              case 'h':
                messageBuf = keyboardUsageString;
                break;
              case 'c':
                break;
              case SDLDisplay::SDLKeySymEscape:
                quitFlag = true;
                break;
              default:
                redrawFlags = 0;
                continue;
            }
            display.clearTextBuffer();
          }
        }
      }
    }
  }
  catch (std::exception& e)
  {
    display.unlockScreenSurface();
    messageBuf += "\033[41m\033[33m\033[1m    Error: ";
    messageBuf += e.what();
    messageBuf += "    ";
    display.printString(messageBuf.c_str());
    display.drawText(0, -1, display.getTextRows(), 1.0f, 1.0f);
    display.blitSurface();
    do
    {
      display.pollEvents(eventBuf, 10, false, false);
      for (size_t i = 0; i < eventBuf.size(); i++)
      {
        if ((eventBuf[i].type() == SDLDisplay::SDLEventWindow &&
             eventBuf[i].data1() == 0) ||
            eventBuf[i].type() == SDLDisplay::SDLEventKeyDown)
        {
          quitFlag = true;
          break;
        }
        else if (eventBuf[i].type() == SDLDisplay::SDLEventWindow &&
                 eventBuf[i].data1() == 1)
        {
          display.blitSurface();
        }
      }
    }
    while (!quitFlag);
    throw;
  }
}


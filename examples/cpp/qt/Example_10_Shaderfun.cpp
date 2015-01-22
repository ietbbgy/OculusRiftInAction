/************************************************************************************

Authors     :   Bradley Austin Davis <bdavis@saintandreas.org>
Copyright   :   Copyright Bradley Austin Davis. All Rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

************************************************************************************/

#ifdef _DEBUG
#define BRAD_DEBUG 1
#endif

#include "Common.h"
#include "Shadertoy.h"
#include "ShadertoyQt.h"

#include <QQuickTextDocument>
#include <QGuiApplication>
#include <QQmlContext>
#include <QNetworkAccessManager>
#include <QtNetwork>
#include <QDeclarativeEngine>

#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#else
#include <oglplus/images/png.hpp>
#endif

#include "TrackerbirdConfig.h"
#include "ShadertoyConfig.h"

#ifdef TRACKERBIRD_PRODUCT_ID
#include <Trackerbird.h>
#endif

const char * ORG_NAME = "Oculus Rift in Action";
const char * ORG_DOMAIN = "oculusriftinaction.com";
const char * APP_NAME = "ShadertoyVR";

QDir CONFIG_DIR;
QSharedPointer<QFile> LOG_FILE;
QtMessageHandler ORIGINAL_MESSAGE_HANDLER;
static const QString SHADERTOY_API_URL = "https://www.shadertoy.com/api/v1/shaders";
static const QString SHADERTOY_MEDIA_URL = "https://www.shadertoy.com/media/shaders/";

using namespace oglplus;

static const float ROOT_2 = sqrt(2.0f);
static const float INV_ROOT_2 = 1.0f / ROOT_2;
static uvec2 UI_SIZE(1280, 720);
static float UI_ASPECT = aspect(vec2(UI_SIZE));
static float UI_INVERSE_ASPECT = 1.0f / UI_ASPECT;

typedef std::shared_ptr<oglplus::VertexShader> VertexShaderPtr;
typedef std::shared_ptr<oglplus::FragmentShader> FragmentShaderPtr;

static ImagePtr loadImageWithAlpha(const std::vector<uint8_t> & data, bool flip) {
  using namespace oglplus;
#ifdef HAVE_OPENCV
  cv::Mat image = cv::imdecode(data, cv::IMREAD_UNCHANGED);
  if (flip) {
    cv::flip(image, image, 0);
  }
  ImagePtr result(new images::Image(image.cols, image.rows, 1, 4, image.data,
    PixelDataFormat::BGRA, PixelDataInternalFormat::RGBA8));
  return result;
#else
  std::stringstream stream(std::string((const char*)&data[0], data.size()));
  return ImagePtr(new images::PNGImage(stream));
#endif
}
static TexturePtr loadCursor(Resource res) {
  using namespace oglplus;
  TexturePtr texture(new Texture());
  Context::Bound(TextureTarget::_2D, *texture)
    .MagFilter(TextureMagFilter::Linear)
    .MinFilter(TextureMinFilter::Linear);

  ImagePtr image = loadImageWithAlpha(Platform::getResourceByteVector(res), true);
  // FIXME detect alignment properly, test on both OpenCV and LibPNG
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  Texture::Storage2D(TextureTarget::_2D, 1, PixelDataInternalFormat::RGBA8, image->Width() * 2, image->Height() * 2);
  {
    size_t size = image->Width() * 2 * image->Height() * 2 * 4;
    uint8_t * empty = new uint8_t[size]; memset(empty, 0, size);
    images::Image blank(image->Width() * 2, image->Height() * 2, 1, 4, empty);
    Texture::SubImage2D(TextureTarget::_2D, blank, 0, 0);
  }
  Texture::SubImage2D(TextureTarget::_2D, *image, image->Width(), 0);
  DefaultTexture().Bind(TextureTarget::_2D);
  return texture;
}

QString readFileToString(const QString & fileName) {
  QFile f(fileName);
  f.open(QFile::ReadOnly);
  QByteArray ba = f.readAll();
  return QString(ba);
}


class ShadertoyRenderer : public QRiftWindow {
  Q_OBJECT
protected:
  struct Channel {
    oglplus::Texture::Target target;
    TexturePtr texture;
    vec3 resolution;
  };
  struct TextureData {
    TexturePtr tex;
    uvec2 size;
  };

  typedef std::map<QUrl, TextureData> TextureMap;
  typedef std::map<QUrl, QUrl> CanonicalUrlMap;
  CanonicalUrlMap canonicalUrlMap;
  TextureMap textureCache;

  // The currently active input channels
  Channel channels[4];
  QUrl channelSources[4];
  bool vrMode{ false };

  // The shadertoy rendering resolution scale.  1.0 means full resolution 
  // as defined by the Oculus SDK as the ideal offscreen resolution 
  // pre-distortion
  float texRes{ 1.0f };

  float eyePosScale{ 1.0f };
  float startTime{ 0.0f };
  // The current fragment source
  LambdaList uniformLambdas;
  // Contains the current 'camera position'
  vec3 position;
  // Geometry for the skybox used to render the scene
  ShapeWrapperPtr skybox;
  // A vertex shader shader, constant throughout the application lifetime
  VertexShaderPtr vertexShader;
  // The fragment shader used to render the shadertoy effect, as loaded 
  // from a preset or created or edited by the user
  FragmentShaderPtr fragmentShader;
  // The compiled shadertoy program
  ProgramPtr shadertoyProgram;

  virtual void setup() {
    QRiftWindow::setup();
    initTextureCache();

    setShaderSourceInternal(oria::qt::toString(Resource::SHADERTOY_SHADERS_DEFAULT_FS));
    assert(shadertoyProgram);
    skybox = oria::loadSkybox(shadertoyProgram);

    Platform::addShutdownHook([&] {
      textureCache.clear();
      shadertoyProgram.reset();
      vertexShader.reset();
      fragmentShader.reset();
      skybox.reset();
    });
  }

  void initTextureCache() {
    using namespace shadertoy;

    for (int i = 0; i < MAX_TEXTURES; ++i) {
      Resource res = TEXTURES[i];
      if (NO_RESOURCE == res) {
        continue;
      }
      QString path = QString(Resources::getResourceMnemonic(res).c_str());
      QString fileName = path.split("/").back();
      QUrl url(QString("qrc:/") + path);
      TextureData & cacheEntry = textureCache[url];
      cacheEntry.tex = oria::load2dTexture(res, cacheEntry.size);

      // Backward compatibility
      canonicalUrlMap[QUrl(QString().sprintf("preset://tex/%d", i))] = url;
      canonicalUrlMap[QUrl(QString().sprintf("preset://tex/%02d", i))] = url;
      canonicalUrlMap[QUrl("/presets/" + fileName)] = url;
    }
    for (int i = 0; i < MAX_CUBEMAPS; ++i) {
      Resource res = CUBEMAPS[i];
      if (NO_RESOURCE == res) {
        continue;
      }
      static int resourceOrder[] = {
        0, 1, 2, 3, 4, 5
      };
      QString path = QString(Resources::getResourceMnemonic(res).c_str());
      QString fileName = path.split("/").back();
      QUrl url(QString("qrc:/") + path);
      uvec2 size;
      TextureData & cacheEntry = textureCache[url];
      cacheEntry.tex = oria::loadCubemapTexture(res, resourceOrder, false);

      // Backward compatibility
      canonicalUrlMap[QUrl(QString().sprintf("preset://cube/%d", i))] = url;
      canonicalUrlMap[QUrl(QString().sprintf("preset://cube/%02d", i))] = url;
      canonicalUrlMap[QUrl("/presets/" + fileName)] = url;
    }
    //std::for_each(canonicalUrlMap.begin(), canonicalUrlMap.end(), [&](CanonicalUrlMap::const_reference & entry) {
    //  qDebug() << entry.second << "\t" << entry.first;
    //});
  }

  void renderShadertoy() {
    Context::Clear().ColorBuffer();
    if (!shadertoyProgram) {
      return;
    }
    MatrixStack & mv = Stacks::modelview();
    mv.withPush([&] {
      mv.untranslate();
      oria::renderGeometry(skybox, shadertoyProgram, uniformLambdas);
    });
    for (int i = 0; i < 4; ++i) {
      oglplus::DefaultTexture().Active(0);
      DefaultTexture().Bind(Texture::Target::_2D);
      DefaultTexture().Bind(Texture::Target::CubeMap);
    }
    oglplus::Texture::Active(0);
  }

  void updateUniforms() {
    using namespace shadertoy;
    std::map<std::string, GLuint> activeUniforms = oria::getActiveUniforms(shadertoyProgram);
    shadertoyProgram->Bind();
    //    UNIFORM_DATE;
    for (int i = 0; i < 4; ++i) {
      const char * uniformName = shadertoy::UNIFORM_CHANNELS[i];
      if (activeUniforms.count(uniformName)) {
        context()->functions()->glUniform1i(activeUniforms[uniformName], i);
      }
    }
    NoProgram().Bind();

    uniformLambdas.clear();
    if (activeUniforms.count(UNIFORM_GLOBALTIME)) {
      uniformLambdas.push_back([&] {
        Uniform<GLfloat>(*shadertoyProgram, UNIFORM_GLOBALTIME).Set(Platform::elapsedSeconds() - startTime);
      });
    }

    if (activeUniforms.count(UNIFORM_RESOLUTION)) {
      uniformLambdas.push_back([&] {
        vec3 res = vec3(renderSize(), 0);
        Uniform<vec3>(*shadertoyProgram, UNIFORM_RESOLUTION).Set(res);
      });
    }

#ifdef USE_RIFT
    if (activeUniforms.count(shadertoy::UNIFORM_POSITION)) {
      uniformLambdas.push_back([&] {
          Uniform<vec3>(*shadertoyProgram, shadertoy::UNIFORM_POSITION).Set(
            (ovr::toGlm(getEyePose().Position) + position) * eyePosScale);
      });
    }
#endif

    for (int i = 0; i < 4; ++i) {
      if (activeUniforms.count(UNIFORM_CHANNELS[i]) && channels[i].texture) {
        uniformLambdas.push_back([=] {
          if (this->channels[i].texture) {
            Texture::Active(i);
            this->channels[i].texture->Bind(channels[i].target);
          }
        });
      }
    }
  }

  vec2 textureSize() {
#ifdef USE_RIFT
    return vec2(ovr::toGlm(eyeTextures[0].Header.TextureSize));
#else
    return vec2(size().width(), size().height());
#endif
  }

  uvec2 renderSize() {
    return uvec2(texRes * textureSize());
  }



  virtual bool setShaderSourceInternal(QString source) {
    try {
      position = vec3();
      if (!vertexShader) {
        vertexShader = VertexShaderPtr(new VertexShader());
        vertexShader->Source(Platform::getResourceString(Resource::SHADERTOY_SHADERS_DEFAULT_VS));
        vertexShader->Compile();
      }

      QString header = shadertoy::SHADER_HEADER;
      for (int i = 0; i < 4; ++i) {
        const Channel & channel = channels[i];
        QString line; line.sprintf("uniform sampler%s iChannel%d;\n",
          channel.target == Texture::Target::CubeMap ? "Cube" : "2D", i);
        header += line;
      }
      header += shadertoy::LINE_NUMBER_HEADER;
      FragmentShaderPtr newFragmentShader(new FragmentShader());
      vrMode = source.contains("#pragma vr");
      source.
        replace(QRegExp("\\bgl_FragColor\\b"), "FragColor").
        replace(QRegExp("\\btexture2D\\b"), "texture").
        replace(QRegExp("\\btextureCube\\b"), "texture");
      source.insert(0, header);
      QByteArray qb = source.toLocal8Bit();
      GLchar * fragmentSource = (GLchar*)qb.data();
      StrCRef src(fragmentSource);
      newFragmentShader->Source(GLSLSource(src));
      newFragmentShader->Compile();
      ProgramPtr result(new Program());
      result->AttachShader(*vertexShader);
      result->AttachShader(*newFragmentShader);

      result->Link();
      shadertoyProgram.swap(result);
      if (!skybox) {
        skybox = oria::loadSkybox(shadertoyProgram);
      }
      fragmentShader.swap(newFragmentShader);
      updateUniforms();
      startTime = Platform::elapsedSeconds();
      emit compileSuccess();
    } catch (ProgramBuildError & err) {
      emit compileError(QString(err.Log().c_str()));
      return false;
    }
    return true;
  }

  virtual TexturePtr loadTexture(const QUrl & source) {
    qDebug() << "Looking for texture " << source;
    QUrl url = source;
    while (canonicalUrlMap.count(url)) {
      url = canonicalUrlMap[url];
    }

    if (!textureCache.count(url)) {
      qWarning() << "Texture " << source << " not found, loading";
      // FIXME
      QFile f(source.toLocalFile());
      f.open(QFile::ReadOnly);
      QByteArray ba = f.readAll();
      std::vector<uint8_t> v;  v.assign(ba.constData(), ba.constData() + ba.size());
      textureCache[url].tex = oria::load2dTexture(v);
    }
    return textureCache[url].tex;
  }

  virtual void setChannelTextureInternal(int channel, shadertoy::ChannelInputType type, const QUrl & textureSource) {
    using namespace oglplus;
    if (textureSource == channelSources[channel]) {
      return;
    }

    channelSources[channel] = textureSource;

    if (QUrl() == textureSource) {
      channels[channel].texture.reset();
      channels[channel].target = Texture::Target::_2D;
      return;
    }

    Channel newChannel;
    uvec2 size;
    switch (type) {
    case shadertoy::ChannelInputType::TEXTURE:
      newChannel.texture = loadTexture(textureSource);
      newChannel.target = Texture::Target::_2D;
      break;

    case shadertoy::ChannelInputType::CUBEMAP:
      newChannel.texture = loadTexture(textureSource);
      newChannel.target = Texture::Target::CubeMap;
      break;

    case shadertoy::ChannelInputType::VIDEO:
      // FIXME, not supported
      break;

    case shadertoy::ChannelInputType::AUDIO:
      // FIXME, not supported
      break;
    }

    channels[channel] = newChannel;
  }

  virtual void setShaderInternal(const shadertoy::Shader & shader) {
    for (int i = 0; i < shadertoy::MAX_CHANNELS; ++i) {
      setChannelTextureInternal(i, shader.channelTypes[i], QString(shader.channelTextures[i].c_str()));
    }
    setShaderSourceInternal(shader.fragmentSource.c_str());
  }

signals:
  void compileError(const QString & source);
  void compileSuccess();
};


class ShadertoyFetcher : public QObject {
  Q_OBJECT

  QQueue<QString> shadersToFetch;
  QNetworkAccessManager qnam;
  QTimer timer;
  int currentNetworkRequests{ 0 };

  virtual void fetchUrl(QUrl url, std::function<void(QByteArray)> f) {
    QNetworkRequest request(url);
    qDebug() << "Requesting url " << url;
    request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, "ShadertoyVR/1.0");
    ++currentNetworkRequests;
    QNetworkReply * netReply = qnam.get(request);
    connect(netReply, static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
      this, [&, url](QNetworkReply::NetworkError code) {
      qWarning() << "Got error " << code << " fetching url " << url;
    });
    connect(netReply, &QNetworkReply::finished, this, [&, f, netReply, url] {
      --currentNetworkRequests;
      qDebug() << "Got response for url " << url;
      QByteArray replyBuffer = netReply->readAll();
      f(replyBuffer);
    });
  }

  void fetchFile(const QUrl & url, const QString & path) {
    fetchUrl(url, [&, path](const QByteArray & replyBuffer) {
      QFile outputFile(path);
      outputFile.open(QIODevice::WriteOnly);
      outputFile.write(replyBuffer);
      outputFile.close();
    });
  }

  virtual void fetchNextShader() {
#ifdef SHADERTOY_API_KEY
    while (!shadersToFetch.empty() && currentNetworkRequests <= 4) {
      QString nextShaderId = shadersToFetch.front();
      shadersToFetch.pop_front();
      QString shaderFile = CONFIG_DIR.absoluteFilePath("shadertoy/" + nextShaderId + ".json");
      QString shaderPreviewFile = CONFIG_DIR.absoluteFilePath("shadertoy/" + nextShaderId + ".jpg");
      if (QFile(shaderFile).exists() && QFile(shaderPreviewFile).exists()) {
        continue;
      }

      if (!QFile(shaderFile).exists()) {
        qDebug() << "Fetching shader " << nextShaderId;
        QUrl url(SHADERTOY_API_URL + QString().sprintf("/%s?key=%s", nextShaderId.toLocal8Bit().constData(), SHADERTOY_API_KEY));
        fetchUrl(url, [&, shaderFile](const QByteArray & replyBuffer) {
          QFile outputFile(shaderFile);
          outputFile.open(QIODevice::WriteOnly);
          outputFile.write(replyBuffer);
          outputFile.close();
        });
      }

      if (!QFile(shaderPreviewFile).exists()) {
        fetchFile(QUrl(SHADERTOY_MEDIA_URL + nextShaderId + ".jpg"), shaderPreviewFile);
      }
    }

    if (shadersToFetch.isEmpty()) {
      timer.stop();
      return;
    }
#endif
  }

public:
  ShadertoyFetcher() {
    connect(&timer, &QTimer::timeout, this, [&] {
      fetchNextShader();
    });
    CONFIG_DIR.mkpath("shadertoy");
  }

  virtual void fetchNetworkShaders() {
#ifdef SHADERTOY_API_KEY
    qDebug() << "Fetching shader list";
    QUrl url(SHADERTOY_API_URL + QString().sprintf("?key=%s", SHADERTOY_API_KEY));
    fetchUrl(url, [&](const QByteArray & replyBuffer) {
      QJsonDocument jsonResponse = QJsonDocument::fromJson(replyBuffer);
      QJsonObject jsonObject = jsonResponse.object();
      QJsonArray shaders = jsonObject["Results"].toArray();
      for (int i = 0; i < shaders.count(); ++i) {
        QString shaderId = shaders.at(i).toString();
        shadersToFetch.push_back(shaderId);
      }
      timer.start(1000);
    });
#endif
  }
};

class ShadertoyWindow : public ShadertoyRenderer {
  Q_OBJECT
  
  typedef std::atomic<GLuint> AtomicGlTexture;
  typedef std::pair<GLuint, GLsync> SyncPair;
  typedef std::queue<SyncPair> TextureTrashcan;
  typedef std::atomic<QPointF> ActomicVec2;
  typedef std::vector<GLuint> TextureDeleteQueue;
  typedef std::mutex Mutex;
  typedef std::unique_lock<Mutex> Lock;
  // A cache of all the input textures available 
  QDir configPath;
  QSettings settings;

  shadertoy::Shader activeShader;

  //////////////////////////////////////////////////////////////////////////////
  // 
  // Offscreen UI
  //
  QOffscreenUi * uiWindow{ new QOffscreenUi() };
  GlslHighlighter highlighter;

  int activePresetIndex{ 0 };
  float savedEyePosScale{ 1.0f };
  vec2 windowSize;
  //////////////////////////////////////////////////////////////////////////////
  // 
  // Shader Rendering information
  //

  // We actually render the shader to one FBO for dynamic framebuffer scaling,
  // while leaving the actual texture we pass to the Oculus SDK fixed.
  // This allows us to have a clear UI regardless of the shader performance
  FramebufferWrapperPtr shaderFramebuffer;

  // The current mouse position as reported by the main thread
  ActomicVec2 mousePosition;
  bool uiVisible{ false };


  // A wrapper for passing the UI texture from the app to the widget
  AtomicGlTexture uiTexture{ 0 };
  TextureTrashcan textureTrash;
  TextureDeleteQueue textureDeleteQueue;
  Mutex textureLock;
  QTimer timer;

  // GLSL and geometry for the UI
  ProgramPtr uiProgram;
  ShapeWrapperPtr uiShape;
  TexturePtr mouseTexture;
  ShapeWrapperPtr mouseShape;

  // For easy compositing the UI texture and the mouse texture
  FramebufferWrapperPtr uiFramebuffer;

  // Geometry and shader for rendering the possibly low res shader to the main framebuffer
  ProgramPtr planeProgram;
  ShapeWrapperPtr plane;

  // Measure the FPS for use in dynamic scaling
  GLuint exchangeUiTexture(GLuint newUiTexture) {
    return uiTexture.exchange(newUiTexture);
  }

  ShadertoyFetcher fetcher;

public:
  ShadertoyWindow() {
    // Fixes an occasional crash caused by a race condition between the Rift 
    // render thread and the UI thread, triggered when Rift swapbuffers overlaps 
    // with the UI thread binding a new FBO (specifically, generating a texture 
    // for the FBO.  
    // Perhaps I should just create N FBOs and have the UI object iterate over them
    {
      QString configLocation = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
      configPath = QDir(configLocation);
      configPath.mkpath("shaders");
    }

    fetcher.fetchNetworkShaders();

    connect(&timer, &QTimer::timeout, this, &ShadertoyWindow::onTimer);
    timer.start(100);
    setupOffscreenUi();
    onLoadPreset(0);
    Platform::addShutdownHook([&] {
      shaderFramebuffer.reset();
      uiProgram.reset();
      uiShape.reset();
      uiFramebuffer.reset();
      mouseTexture.reset();
      mouseShape.reset();
      uiFramebuffer.reset();
      planeProgram.reset();
      plane.reset();
    });
  }

  virtual void stop() {
    ShadertoyRenderer::stop();
    delete uiWindow;
    uiWindow = nullptr;
  }

private:
  virtual void setup() {
    ShadertoyRenderer::setup();

    // The geometry and shader for rendering the 2D UI surface when needed
    uiProgram = oria::loadProgram(
      Resource::SHADERS_TEXTURED_VS,
      Resource::SHADERS_TEXTURED_FS);
    uiShape = oria::loadPlane(uiProgram, UI_ASPECT);

    // The geometry and shader for scaling up the rendered shadertoy effect
    // up to the full offscreen render resolution.  This is then compositied 
    // with the UI window
    planeProgram = oria::loadProgram(
      Resource::SHADERS_TEXTURED_VS,
      Resource::SHADERS_TEXTURED_FS);
    plane = oria::loadPlane(planeProgram, 1.0);

    mouseTexture = loadCursor(Resource::IMAGES_CURSOR_PNG);
    mouseShape = oria::loadPlane(uiProgram, UI_INVERSE_ASPECT);

    uiFramebuffer = FramebufferWrapperPtr(new FramebufferWrapper());
    uiFramebuffer->init(UI_SIZE);

    shaderFramebuffer = FramebufferWrapperPtr(new FramebufferWrapper());
    shaderFramebuffer->init(textureSize());

    DefaultFramebuffer().Bind(Framebuffer::Target::Draw);
  }

  void setupOffscreenUi() {
#ifdef USE_RIFT
    this->endFrameLock = &uiWindow->renderLock;
#endif
    qApp->setFont(QFont("Arial", 14, QFont::Bold));
    uiWindow->pause();
    uiWindow->setup(QSize(UI_SIZE.x, UI_SIZE.y), context());
    {
      QStringList dataList;
      for (int i = 0; i < shadertoy::MAX_PRESETS; ++i) {
        dataList.append(shadertoy::PRESETS[i].name);
      }
      auto qmlContext = uiWindow->m_qmlEngine->rootContext();
      qmlContext->setContextProperty("presetsModel", QVariant::fromValue(dataList));
      QUrl url = QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/shaders");
      qmlContext->setContextProperty("userPresetsFolder", url);
    }
    uiWindow->setProxyWindow(this);

#ifdef _DEBUG
    //	QUrl qml = QUrl::fromLocalFile("/Users/bradd/git/OculusRiftInAction/resources/shadertoy/Combined.qml");
    QUrl qml = QUrl::fromLocalFile("C:\\Users\\bdavis\\Git\\OculusRiftExamples\\resources\\shadertoy\\Combined.qml");
#else
    QUrl qml = QUrl("qrc:/shadertoy/Combined.qml");
    uiWindow->m_qmlEngine->addImportPath("./qml");
    uiWindow->m_qmlEngine->addImportPath(".");
#endif
    uiWindow->loadQml(qml);
    connect(uiWindow, &QOffscreenUi::textureUpdated, this, [&](int textureId) {
      uiWindow->lockTexture(textureId);
      GLuint oldTexture = exchangeUiTexture(textureId);
      if (oldTexture) {
        uiWindow->releaseTexture(oldTexture);
      }
    });

    QQuickItem * editorControl;
    editorControl = uiWindow->m_rootItem->findChild<QQuickItem*>("shaderTextEdit");
    if (editorControl) {
      highlighter.setDocument(
                              editorControl->property("textDocument").value<QQuickTextDocument*>()->textDocument());
    }

    QObject::connect(uiWindow->m_rootItem, SIGNAL(toggleUi()),
      this, SLOT(onToggleUi()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(channelTextureChanged(int, int, QString)),
      this, SLOT(onChannelTextureChanged(int, int, QString)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(shaderSourceChanged(QString)),
      this, SLOT(onShaderSourceChanged(QString)));

    // FIXME add confirmation for when the user might lose edits.
    QObject::connect(uiWindow->m_rootItem, SIGNAL(loadPreset(int)),
      this, SLOT(onLoadPreset(int)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(loadNextPreset()),
      this, SLOT(onLoadNextPreset()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(loadPreviousPreset()),
      this, SLOT(onLoadPreviousPreset()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(loadShaderFile(QString)),
      this, SLOT(onLoadShaderFile(QString)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(saveShaderXml(QString)),
      this, SLOT(onSaveShaderXml(QString)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(recenterPose()),
      this, SLOT(onRecenterPosition()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(modifyTextureResolution(double)),
      this, SLOT(onModifyTextureResolution(double)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(modifyPositionScale(double)),
      this, SLOT(onModifyPositionScale(double)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(resetPositionScale()),
      this, SLOT(onResetPositionScale()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(toggleEyePerFrame()),
      this, SLOT(onToggleEyePerFrame()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(epfModeChanged(bool)),
      this, SLOT(onEpfModeChanged(bool)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(startShutdown()),
      this, SLOT(onShutdown()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(restartShader()),
      this, SLOT(onRestartShader()));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(newShaderFilepath(QString)),
      this, SLOT(onNewShaderFilepath(QString)));
    QObject::connect(uiWindow->m_rootItem, SIGNAL(newShaderHighlighted(QString)),
      this, SLOT(onNewShaderHighlighted(QString)));

    QObject::connect(this, &ShadertoyWindow::compileSuccess, this, [&] {
      setItemProperty("errorFrame", "height", 0);
      setItemProperty("errorFrame", "visible", false);
      setItemProperty("compileErrors", "text", "");
      setItemProperty("shaderTextFrame", "errorMargin", 0);
    });

    QObject::connect(this, &ShadertoyWindow::compileError, this, [&](const QString& errors){
      setItemProperty("errorFrame", "height", 128);
      setItemProperty("errorFrame", "visible", true);
      setItemProperty("compileErrors", "text", errors);
      setItemProperty("shaderTextFrame", "errorMargin", 8);
    });

    connect(this, &ShadertoyWindow::fpsUpdated, this, [&](float fps) {
      setItemText("fps", QString().sprintf("%0.0f", fps));
    });

    setItemText("res", QString().sprintf("%0.2f", texRes));
  }

  QVariant getItemProperty(const QString & itemName, const QString & property) {
    QQuickItem * item = uiWindow->m_rootItem->findChild<QQuickItem*>(itemName);
    if (nullptr != item) {
      return item->property(property.toLocal8Bit());
    } else {
      qWarning() << "Could not find item " << itemName << " on which to set property " << property;
    }
    return QVariant();
  }

  void setItemProperty(const QString & itemName, const QString & property, const QVariant & value) {
    QQuickItem * item = uiWindow->m_rootItem->findChild<QQuickItem*>(itemName);
    if (nullptr != item) {
      bool result = item->setProperty(property.toLocal8Bit(), value);
      if (!result) {
        qWarning() << "Set property " << property << " on item " << itemName << " returned " << result;
      }
    } else {
      qWarning() << "Could not find item " << itemName << " on which to set property " << property;
    }
  }

  void setItemText(const QString & itemName, const QString & text) {
    setItemProperty(itemName, "text", text);
  }

  QString getItemText(const QString & itemName) {
    return getItemProperty(itemName, "text").toString();
  }

private slots:
  void onToggleUi() {
	  uiVisible = !uiVisible;
	  setItemProperty("shaderTextEdit", "readOnly", !uiVisible);
	  if (uiVisible) {
		savedEyePosScale = eyePosScale;
		eyePosScale = 0.0f;
    uiWindow->resume();
	  } else {
		eyePosScale = savedEyePosScale;
    uiWindow->pause();
	  }
  }

  void onLoadNextPreset() {
    int newPreset = (activePresetIndex + 1) % shadertoy::MAX_PRESETS;
    onLoadPreset(newPreset);
  }

  void onFontSizeChanged(int newSize) {
    settings.setValue("fontSize", newSize);
  }

  void onLoadPreviousPreset() {
    int newPreset = (activePresetIndex + shadertoy::MAX_PRESETS - 1) % shadertoy::MAX_PRESETS;
    onLoadPreset(newPreset);
  }

  void onLoadPreset(int index) {
    activePresetIndex = index;
    auto preset = shadertoy::PRESETS[index];
    QString shaderPath = Resources::getResourceMnemonic(preset.res).c_str();
    if (shaderPath.endsWith(".xml", Qt::CaseInsensitive)) {
      loadShader(shadertoy::loadShaderXml(preset.res));
    } else if (shaderPath.endsWith(".json", Qt::CaseInsensitive)) {
      loadShader(shadertoy::loadShaderJson(preset.res));
    } else {
      qWarning() << "Don't know how to parse path " << shaderPath; 
    }
  }

  void onLoadShaderFile(const QString & shaderPath) {
    qDebug() << "Loading shader from " << shaderPath;
    if (shaderPath.endsWith(".xml")) {
      loadShader(shadertoy::loadShaderXml(shaderPath));
    } else if (shaderPath.endsWith(".json")) {
      loadShader(shadertoy::loadShaderJson(shaderPath));
    }
  }

  void onNewShaderFilepath(const QString & shaderPath) {
    QDir newDir(shaderPath);
    QUrl url = QUrl::fromLocalFile(newDir.absolutePath());
//    setItemProperty("userPresetsModel", "folder", url);
    auto qmlContext = uiWindow->m_qmlEngine->rootContext();
    qmlContext->setContextProperty("userPresetsFolder", url);
  }
  
  void onNewShaderHighlighted(const QString & shaderPath) {
    qDebug() << "New shader highlighted " << shaderPath;
    QString previewPath = shaderPath;
    previewPath.replace(QRegularExpression("\\.(json|xml)$"), ".jpg");
    setItemProperty("previewImage", "source", QFile::exists(previewPath) ? QUrl::fromLocalFile(previewPath) : QUrl());
    if (shaderPath.endsWith(".json")) {
      setItemProperty("loadRoot", "activeShaderString", readFileToString(shaderPath));
    } else {
      setItemProperty("loadRoot", "activeShaderString", "");
    }
  }

  void onSaveShaderXml(const QString & shaderPath) {
    Q_ASSERT(!shaderPath.isEmpty());
    //shadertoy::saveShaderXml(activeShader);
    activeShader.name = shaderPath.toLocal8Bit();
    activeShader.fragmentSource = getItemText("shaderTextEdit").toLocal8Bit();
    QString destinationFile = configPath.absoluteFilePath(QString("shaders/")
      + shaderPath
      + ".xml");
    qDebug() << "Saving shader to " << destinationFile;
    shadertoy::saveShaderXml(destinationFile, activeShader);
  }

  void onChannelTextureChanged(const int & channelIndex, const int & channelType, const QString & texturePath) {
    queueRenderThreadTask([&, channelIndex, channelType, texturePath] {
      activeShader.channelTypes[channelIndex] = (shadertoy::ChannelInputType)channelType;
      activeShader.channelTextures[channelIndex] = texturePath.toLocal8Bit();
      setChannelTextureInternal(channelIndex, 
        (shadertoy::ChannelInputType)channelType, 
        QUrl(texturePath));
      updateUniforms();
    });
  }

  void onShaderSourceChanged(const QString & shaderSource) {
    queueRenderThreadTask([&, shaderSource] {
      setShaderSourceInternal(shaderSource);
      updateUniforms();
    });
  }

  void onModifyTextureResolution(double scale) {
    float newRes = scale * texRes;
    newRes = std::max(0.1f, std::min(1.0f, newRes));
    if (newRes != texRes) {
      queueRenderThreadTask([&, newRes] {
        texRes = newRes;
      });
      setItemText("res", QString().sprintf("%0.2f", newRes));
    }
  }

  void onRecenterPosition() {
#ifdef USE_RIFT
    queueRenderThreadTask([&] {
      ovrHmd_RecenterPose(hmd);
    });
#endif
  }

  void onModifyTextureResolution(double scale) {
    float newRes = scale * texRes;
    newRes = std::max(0.1f, std::min(1.0f, newRes));
    if (newRes != texRes) {
      queueRenderThreadTask([&, newRes] {
        texRes = newRes;
      });
      setItemText("res", QString().sprintf("%0.2f", newRes));
    }
  }

  void onModifyPositionScale(double scale) {
    float newPosScale = scale * eyePosScale;
    queueRenderThreadTask([&, newPosScale] {
      eyePosScale = newPosScale;
    });
    // FIXME update the UI
    setItemText("eps", QString().sprintf("%0.2f", newPosScale));
  }

  void onResetPositionScale() {
    queueRenderThreadTask([&] {
      eyePosScale = 1.0f;
    });
    // FIXME update the UI
    setItemText("eps", QString().sprintf("%0.2f", 1.0f));
  }

  void onToggleEyePerFrame() {
#ifdef USE_RIFT
    onEpfModeChanged(!eyePerFrameMode);
#endif
  }

  void onEpfModeChanged(bool checked) {
    bool newEyePerFrameMode = checked;
#ifdef USE_RIFT
    queueRenderThreadTask([&, newEyePerFrameMode] {
      eyePerFrameMode = newEyePerFrameMode;
    });
#endif
    setItemProperty("epf", "checked", newEyePerFrameMode);
  }

  void onRestartShader() {
    queueRenderThreadTask([&] {
      startTime = Platform::elapsedSeconds();
    });
  }

  void onShutdown() {
    QApplication::instance()->quit();
  }

  void onTimer() {
    TextureDeleteQueue tempTextureDeleteQueue;

    // Scope the lock tightly
    {
      Lock lock(textureLock);
      if (!textureDeleteQueue.empty()) {
        tempTextureDeleteQueue.swap(textureDeleteQueue);
      }
    }

    if (!tempTextureDeleteQueue.empty()) {
      std::for_each(tempTextureDeleteQueue.begin(), tempTextureDeleteQueue.end(), [&] (int usedTexture){
        uiWindow->releaseTexture(usedTexture);
      });
    }
  }

private:
  void loadShader(const shadertoy::Shader & shader) {
    assert(!shader.fragmentSource.empty());
    activeShader = shader;
    setItemText("shaderTextEdit", QString(shader.fragmentSource.c_str()));
    setItemText("shaderName", QString(shader.name.c_str()));
    for (int i = 0; i < 4; ++i) {
      QUrl url = QString(activeShader.channelTextures[i].c_str());
      while (canonicalUrlMap.count(url)) {
        url = canonicalUrlMap[url];
      }
      setItemProperty(QString().sprintf("channel%d", i), "source",  url);
    }
    // FIXME update the channel texture buttons
    queueRenderThreadTask([&, shader] {
      setShaderInternal(shader);
      updateUniforms();
    });
  }

  void loadFile(const QString & file) {
    loadShader(shadertoy::loadShaderXml(file));
  }

  void updateFps(float fps) {
    emit fpsUpdated(fps);
  }

  ///////////////////////////////////////////////////////
  //
  // Event handling customization
  // 
  void mouseMoveEvent(QMouseEvent * me) { 
    // Make sure we don't show the system cursor over the window
    qApp->setOverrideCursor(QCursor(Qt::BlankCursor));
    // Interpret the mouse position as NDC coordinates
    QPointF mp = me->localPos();
    mp.rx() /= size().width();
    mp.ry() /= size().height();
    mp *= 2.0f;
    mp -= QPointF(1.0f, 1.0f);
    mp.ry() *= -1.0f;
    mousePosition.store(mp);
    QRiftWindow::mouseMoveEvent(me);
  }

  bool event(QEvent * e) {
    static bool dismissedHmd = false;
    switch (e->type()) {
    case QEvent::KeyPress:
#ifdef USE_RIFT
      if (!dismissedHmd) {
        // Allow the user to remove the HSW message early
        ovrHSWDisplayState hswState;
        ovrHmd_GetHSWDisplayState(hmd, &hswState);
        if (hswState.Displayed) {
          ovrHmd_DismissHSWDisplay(hmd);
          dismissedHmd = true;
          return true;
        }
      }
#endif
        
    case QEvent::KeyRelease:
    {
      if (QApplication::sendEvent(uiWindow->m_quickWindow, e)) {
        return true;
      }
    }
    break;

    case QEvent::Wheel:
    {
      QWheelEvent * we = (QWheelEvent*)e;
      QWheelEvent mappedEvent(mapWindowToUi(we->pos()), we->delta(), we->buttons(), we->modifiers(), we->orientation());
      QCoreApplication::sendEvent(uiWindow->m_quickWindow, &mappedEvent);
      return true;
    }
    break;

    case QEvent::MouseMove:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    {
      QMouseEvent * me = (QMouseEvent *)e;
      QMouseEvent mappedEvent(e->type(), mapWindowToUi(me->localPos()), me->screenPos(), me->button(), me->buttons(), me->modifiers());
      QCoreApplication::sendEvent(uiWindow->m_quickWindow, &mappedEvent);
      return QRiftWindow::event(e);
    }

    default: break;
    }

    return QRiftWindow::event(e);
  }

  void resizeEvent(QResizeEvent *e) {
    windowSize = vec2(e->size().width(), e->size().height());
  }

  QPointF mapWindowToUi(const QPointF & p) {
    vec2 pos = vec2(p.x(), p.y());
    pos /= windowSize;
    pos *= UI_SIZE;
    return QPointF(pos.x, pos.y);
  }

  ///////////////////////////////////////////////////////
  //
  // Rendering functionality
  // 
  void perFrameRender() {
    Context::Enable(Capability::Blend);
    Context::BlendFunc(BlendFunction::SrcAlpha, BlendFunction::OneMinusSrcAlpha);
    Context::Disable(Capability::ScissorTest);
    Context::Disable(Capability::DepthTest);
    Context::Disable(Capability::CullFace);
    if (uiVisible) {
      static GLuint lastUiTexture = 0;
      static GLsync lastUiSync;
      GLuint currentUiTexture = uiTexture.exchange(0);
      if (0 == currentUiTexture) {
        currentUiTexture = lastUiTexture;
      } else {
        // If the texture has changed, push it into the trash bin for
        // deletion once it's finished rendering
        if (lastUiTexture) {
          textureTrash.push(SyncPair(lastUiTexture, lastUiSync));
        }
        lastUiTexture = currentUiTexture;
      }
      MatrixStack & mv = Stacks::modelview();
      if (currentUiTexture) {
        Texture::Active(0);
        // Composite the UI image and the mouse sprite
        uiFramebuffer->Bound([&] {
          Context::Clear().ColorBuffer();
          oria::viewport(UI_SIZE);
          // Clear out the projection and modelview here.
          Stacks::withIdentity([&] {
            glBindTexture(GL_TEXTURE_2D, currentUiTexture);
            oria::renderGeometry(plane, uiProgram);
            
            // Render the mouse sprite on the UI
            QPointF mp = mousePosition.load();
            mv.translate(vec3(mp.x(), mp.y(), 0.0f));
            mv.scale(vec3(0.1f));
            mouseTexture->Bind(Texture::Target::_2D);
            oria::renderGeometry(mouseShape, uiProgram);
          });
        });
        lastUiSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      }
    }
    
    TextureDeleteQueue tempTextureDeleteQueue;
    while (!textureTrash.empty()) {
      SyncPair & top = textureTrash.front();
      GLuint & texture = top.first;
      GLsync & sync = top.second;
      GLenum result = glClientWaitSync(sync, 0, 0);
      if (GL_ALREADY_SIGNALED == result || GL_CONDITION_SATISFIED == result) {
        tempTextureDeleteQueue.push_back(texture);
        textureTrash.pop();
      } else {
        break;
      }
    }
    
    if (!tempTextureDeleteQueue.empty()) {
      Lock lock(textureLock);
      textureDeleteQueue.insert(textureDeleteQueue.end(),
                                tempTextureDeleteQueue.begin(), tempTextureDeleteQueue.end());
    }
  }

  void perEyeRender() {
    // Render the shadertoy effect into a framebuffer, possibly at a 
    // smaller resolution than recommended
    shaderFramebuffer->Bound([&] {
      Context::Clear().ColorBuffer();
      oria::viewport(renderSize());
      renderShadertoy();
    });
    oria::viewport(textureSize());

    // Now re-render the shader output to the screen.
    shaderFramebuffer->BindColor(Texture::Target::_2D);
#ifdef USE_RIFT
    if (vrMode) {
#endif
      // In VR mode, we want to cover the entire surface
      Stacks::withIdentity([&] {
        oria::renderGeometry(plane, planeProgram, LambdaList({ [&] {
          Uniform<vec2>(*planeProgram, "UvMultiplier").Set(vec2(texRes));
        } }));
      });
#ifdef USE_RIFT
    } else {
      // In 2D mode, we want to render it as a window behind the UI
      Context::Clear().ColorBuffer();
      MatrixStack & mv = Stacks::modelview();
      static vec3 scale = vec3(3.0f, 3.0f / (textureSize().x / textureSize().y), 3.0f);
      static vec3 trans = vec3(0, 0, -3.5);
      static mat4 rot = glm::rotate(mat4(), PI / 2.0f, Vectors::Y_AXIS);


      for (int i = 0; i < 4; ++i) {
        mv.withPush([&] {
          for (int j = 0; j < i; ++j) {
            mv.postMultiply(rot);
          }
          mv.translate(trans);
          mv.scale(scale);
          oria::renderGeometry(plane, planeProgram, LambdaList({ [&] {
            Uniform<vec2>(*planeProgram, "UvMultiplier").Set(vec2(texRes));
          } }));
          oria::renderGeometry(plane, planeProgram);
        });
      }
    }
#endif

    if (uiVisible) {
      MatrixStack & mv = Stacks::modelview();
      Texture::Active(0);
//      oria::viewport(textureSize());
      mv.withPush([&] {
        mv.translate(vec3(0, 0, -1));
        uiFramebuffer->BindColor();
        oria::renderGeometry(uiShape, uiProgram);
      });
    }
  }

public slots:

  void onSixDofMotion(const vec3 & tr, const vec3 & mo) {
    SAY("%f, %f, %f", tr.x, tr.y, tr.z);
    queueRenderThreadTask([&, tr, mo] {
      position += tr;
    });
  }

signals:
  void fpsUpdated(float);
};



void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
  ORIGINAL_MESSAGE_HANDLER(type, context, msg);
  QByteArray localMsg = msg.toLocal8Bit();
  QString now = QDateTime::currentDateTime().toString("yyyy.dd.MM_hh:mm:ss");
  switch (type) {
  case QtDebugMsg:
    LOG_FILE->write(QString().sprintf("%s Debug:    %s (%s:%u, %s)\n", now.toLocal8Bit().constData(), localMsg.constData(), context.file, context.line, context.function).toLocal8Bit());
    break;
  case QtWarningMsg:
    LOG_FILE->write(QString().sprintf("%s Warning:  %s (%s:%u, %s)\n", now.toLocal8Bit().constData(), localMsg.constData(), context.file, context.line, context.function).toLocal8Bit());
    break;
  case QtCriticalMsg:
    LOG_FILE->write(QString().sprintf("%s Critical: %s (%s:%u, %s)\n", now.toLocal8Bit().constData(), localMsg.constData(), context.file, context.line, context.function).toLocal8Bit());
    break;
  case QtFatalMsg:
    LOG_FILE->write(QString().sprintf("%s Fatal:    %s (%s:%u, %s)\n", now.toLocal8Bit().constData(), localMsg.constData(), context.file, context.line, context.function).toLocal8Bit());
    LOG_FILE->flush();
    abort();
  }
  LOG_FILE->flush();
}

class ShadertoyApp : public QApplication {
  Q_OBJECT

  QWidget desktopWindow;

public:
  ShadertoyApp(int argc, char ** argv) : QApplication(argc, argv) {
    Q_INIT_RESOURCE(Resource);
    QCoreApplication::setOrganizationName(ORG_NAME);
    QCoreApplication::setOrganizationDomain(ORG_DOMAIN);
    QCoreApplication::setApplicationName(APP_NAME);
#if (!defined(_DEBUG) && defined(TRACKERBIRD_PRODUCT_ID))
    QCoreApplication::setApplicationVersion(QString::fromWCharArray(TRACKERBIRD_PRODUCT_VERSION));
#endif
    CONFIG_DIR = QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    QString currentLogName = CONFIG_DIR.absoluteFilePath("ShadertoyVR.log");
    LOG_FILE = QSharedPointer<QFile>(new QFile(currentLogName));
    if (LOG_FILE->exists()) {
      QFile::rename(currentLogName, 
        CONFIG_DIR.absoluteFilePath("ShadertoyVR_" + 
          QDateTime::currentDateTime().toString("yyyy.dd.MM_hh.mm.ss") + ".log"));
    }
    if (!LOG_FILE->open(QIODevice::WriteOnly | QIODevice::Append)) {
      qWarning() << "Could not open log file";
    }
    ORIGINAL_MESSAGE_HANDLER = qInstallMessageHandler(myMessageOutput);
  }

  virtual ~ShadertoyApp() {
    qInstallMessageHandler(ORIGINAL_MESSAGE_HANDLER);
    LOG_FILE->close();
  }

private:
  void setupDesktopWindow() {
    desktopWindow.setLayout(new QFormLayout());
    QLabel * label = new QLabel("Your Oculus Rift is now active.  Please put on your headset.  Share and enjoy");
    desktopWindow.layout()->addWidget(label);
    desktopWindow.show();
  }
};

MAIN_DECL { 
  try {
#ifdef USE_RIFT
    ovr_Initialize();
#endif

#if (!defined(_DEBUG) && defined(TRACKERBIRD_PRODUCT_ID))
    tbCreateConfig(TRACKERBIRD_URL, TRACKERBIRD_PRODUCT_ID, 
      TRACKERBIRD_PRODUCT_VERSION, TRACKERBIRD_BUILD_NUMBER, 
      TRACKERBIRD_MULTISESSION_ENABLED);
    tbStart();
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "./plugins"); 
    qputenv("QML_IMPORT_PATH", "./qml");
#endif

    QT_APP_WITH_ARGS(ShadertoyApp);

    ShadertoyWindow * riftRenderWidget;
    riftRenderWidget = new ShadertoyWindow();
    riftRenderWidget->start();
    riftRenderWidget->requestActivate();
    int result = app.exec(); 

#if (!defined(_DEBUG) && defined(TRACKERBIRD_PRODUCT_ID))
    tbStop(TRUE);
#endif

    riftRenderWidget->stop();
    riftRenderWidget->makeCurrent();
    Platform::runShutdownHooks();
    delete riftRenderWidget;

    ovr_Shutdown();

    return result;
  } catch (std::exception & error) { 
    SAY_ERR(error.what()); 
  } catch (const std::string & error) { 
    SAY_ERR(error.c_str()); 
  } 
  return -1; 
} 



#include "Example_10_Shaderfun.moc"




#if 0
SiHdl si;
SiGetEventData getEventData;
bool nativeEventFilter(const QByteArray & eventType, void * message, long * result) {
  // On Windows, eventType is set to "windows_generic_MSG" for messages sent 
  // to toplevel windows, and "windows_dispatcher_MSG" for system - wide 
  // messages such as messages from a registered hot key.In both cases, the 
  // message can be casted to a MSG pointer.The result pointer is only used 
  // on Windows, and corresponds to the LRESULT pointer.
  MSG & msg = *(MSG*)message;
  SiSpwEvent siEvent;
  vec3 tr, ro;
  long * md = siEvent.u.spwData.mData;
  SiGetEventWinInit(&getEventData, msg.message, msg.wParam, msg.lParam);
  if (SiGetEvent(si, 0, &getEventData, &siEvent) == SI_IS_EVENT) {
    switch (siEvent.type) {
    case SI_MOTION_EVENT:
      emit sixDof(
        vec3(md[SI_TX], md[SI_TY], -md[SI_TZ]),
        vec3(md[SI_RX], md[SI_RY], md[SI_RZ]));
      break;

    default:
      break;
    }
    return true;
  }
  return false;
}

SpwRetVal result = SiInitialize();
int cnt = SiGetNumDevices();
SiDevID devId = SiDeviceIndex(0);
SiOpenData siData;
SiOpenWinInit(&siData, (HWND)riftRenderWidget.effectiveWinId());
si = SiOpen("app", devId, SI_NO_MASK, SI_EVENT, &siData);
installNativeEventFilter(this);

#endif



/* 
yes: 
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/Xlf3D8.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/XsjXR1.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/XslXW2.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ld2GRz.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldSGRW.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldfGzr.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/4slGzn.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldj3Dm.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/lsl3W2.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/lts3Wn.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/XdBSzd.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/MsBGRh.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/4ds3zn.json"

maybe:

Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/Xll3Wn.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/Xsl3Rl.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldSGzR.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldX3Ws.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldsGWB.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/lsBXD1.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/Xds3zN.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/4sl3zn.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/4sjXzG.json"

debug:
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/ldjXD3.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/XdfXDB.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/Msl3Rr.json"
Loading shader from  "c:/Users/bdavis/AppData/Local/Oculus Rift in Action/ShadertoyVR/shadertoy/4sXGDs.json"

*/

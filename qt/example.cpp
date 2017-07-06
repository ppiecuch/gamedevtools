#define _USE_MATH_DEFINES
#include <QDebug>
#include <QDirIterator>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QWaitCondition>
#include <QPainter>
#include <QWindow>
#include <QOpenGLContext>
#include <QOpenGLPaintDevice>
#include <QOpenGLFunctions>
#include <QOpenGLDebugLogger>
#include <QApplication>

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "debug_font.h"

#define _CRT_SECURE_NO_WARNINGS

#define LIGHTMAPPER_IMPLEMENTATION
#define LM_DEBUG_INTERPOLATION
#include "../lightmapper.h"

#define S2O_IMPLEMENTATION
#include "sproutline.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifndef M_PI // even with _USE_MATH_DEFINES not always available
#define M_PI 3.14159265358979323846
#endif


class SleepSimulator {
	QMutex localMutex;
	QWaitCondition sleepSimulator;
public:
	SleepSimulator() { localMutex.lock(); }
	void sleep(unsigned long sleepMS) { sleepSimulator.wait(&localMutex, sleepMS); }
	void CancelSleep() { sleepSimulator.wakeAll(); }
};

double qtGetTime() {
	static QElapsedTimer timer;
	if (!timer.isValid())
	timer.start();
	return timer.elapsed() / 1000.; 
}

void qtDelay(long ms) {
	SleepSimulator s;
	s.sleep(ms);
}


// == Lightmapper utilities ==

typedef struct {
	float p[3];
	float t[2];
} vertex_t;

typedef struct
{
	GLuint program;
	GLint u_lightmap;
	GLint u_projection;
	GLint u_view;

	GLuint lightmap;
	int w, h;

	GLuint vbo, ibo;
	vertex_t *vertices;
	unsigned short *indices;
	unsigned int vertexCount, indexCount;
} scene_t;

static int initScene(scene_t *scene);
static void drawScene(scene_t *scene, float *view, float *projection);
static void destroyScene(scene_t *scene);
static int bake(scene_t *scene);

static void multiplyMatrices(float *out, float *a, float *b);
static void translationMatrix(float *out, float x, float y, float z);
static void rotationMatrix(float *out, float angle, float x, float y, float z);
static void transformPosition(float *out, float *m, float *p);
static void transposeMatrix(float *out, float *m);
static void perspectiveMatrix(float *out, float fovy, float aspect, float zNear, float zFar);


// == Qt Window ==

class Window : public QWindow, protected QOpenGLFunctions
{
	Q_OBJECT
	enum  {
        Key_W,
        Key_S,
        Key_A,
        Key_D,
        Key_E,
        Key_Q,
        Key_Shift,
        Keys_Active
    };	
private:
	bool m_done, m_update_pending, m_resize_pending, m_auto_refresh;
	QOpenGLContext *m_context;
	QOpenGLPaintDevice *m_device;
	scene_t m_scene;
public:
	QPoint m_cursor; QSize m_size;
    float m_view[16], m_projection[16];
    bool m_mouse_left, m_keys[Keys_Active];
public:
	Window(QWindow *parent = 0) : QWindow(parent)
	, m_update_pending(false)
	, m_resize_pending(false)
	, m_auto_refresh(true)
	, m_context(0)
	, m_device(0)
    , m_scene({0}), m_view{}, m_projection{}, m_mouse_left(0), m_keys{}
	, m_done(false) {
		setSurfaceType(QWindow::OpenGLSurface);
	}
	~Window() { destroyScene(&m_scene); delete m_device; }
	void setAutoRefresh(bool a) { m_auto_refresh = a; }	
    void fpsCameraViewMatrix(float *view, bool left_mouse, bool *keys)
    {
        // initial camera config
        static float position[] = { 0.0f, 0.3f, 1.5f };
        static float rotation[] = { 0.0f, 0.0f };

        // mouse look
        static double lastMouse[] = { 0.0, 0.0 };
        if (left_mouse)
        {
            rotation[0] += (m_cursor.y() - lastMouse[1]) * -0.2f;
            rotation[1] += (m_cursor.x() - lastMouse[0]) * -0.2f;
        }
        lastMouse[0] = m_cursor.x();
        lastMouse[1] = m_cursor.y();

        float rotationY[16], rotationX[16], rotationYX[16];
        rotationMatrix(rotationX, rotation[0], 1.0f, 0.0f, 0.0f);
        rotationMatrix(rotationY, rotation[1], 0.0f, 1.0f, 0.0f);
        multiplyMatrices(rotationYX, rotationY, rotationX);

        // keyboard movement (WSADEQ) + Shift
        float speed = (keys[Key_Shift]) ? 0.1f : 0.01f;
        float movement[3] = {0};
        if (keys[Key_W]) movement[2] -= speed;
        if (keys[Key_S]) movement[2] += speed;
        if (keys[Key_A]) movement[0] -= speed;
        if (keys[Key_D]) movement[0] += speed;
        if (keys[Key_E]) movement[1] -= speed;
        if (keys[Key_Q]) movement[1] += speed;

        float worldMovement[3];
        transformPosition(worldMovement, rotationYX, movement);
        position[0] += worldMovement[0];
        position[1] += worldMovement[1];
        position[2] += worldMovement[2];

        // construct view matrix
        float inverseRotation[16], inverseTranslation[16];
        transposeMatrix(inverseRotation, rotationYX);
        translationMatrix(inverseTranslation, -position[0], -position[1], -position[2]);
        multiplyMatrices(view, inverseTranslation, inverseRotation); // = inverse(translation(position) * rotationYX);
    }
    void render(QPainter *painter) {
		Q_UNUSED(painter);

		glViewport(0, 0, m_size.width() * devicePixelRatio(), m_size.height() * devicePixelRatio());

		// camera for window
		fpsCameraViewMatrix(m_view, m_mouse_left, m_keys);
		perspectiveMatrix(m_projection, 45.0f, float(m_size.width()) / float(m_size.height()), 0.01f, 100.0f);
		
		// draw to screen with a blueish sky
		glClearColor(0.6f, 0.8f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		drawScene(&m_scene, m_view, m_projection);

        dbgFlush();
    }
	void initialize() {
		QOpenGLDebugLogger *m_logger = new QOpenGLDebugLogger(this);
		m_logger->initialize();		
		qDebug() << "OpenGL infos with gl functions:";
		qDebug() << "-------------------------------";
		qDebug() << " Renderer:" << (const char*)glGetString(GL_RENDERER);
		qDebug() << " Vendor:" << (const char*)glGetString(GL_VENDOR);
		qDebug() << " OpenGL Version:" << (const char*)glGetString(GL_VERSION);
		qDebug() << " GLSL Version:" << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        
        setTitle(QString("Qt %1 - %2 (%3)").arg(QT_VERSION_STR).arg((const char*)glGetString(GL_VERSION)).arg((const char*)glGetString(GL_RENDERER)));

        dbgLoadFont();
        dbgSetPixelRatio(devicePixelRatio());

        if (!initScene(&m_scene))
        {
            dbgSetStatusLine("Could not initialize scene.");
        }

        dbgAppendMessage("Ambient Occlusion Baking Example.");
        dbgAppendMessage("Use your mouse and the W, A, S, D, E, Q keys to navigate.");
        dbgAppendMessage("Press SPACE to start baking one light bounce!");
        dbgAppendMessage("This will take a few seconds and bake a lightmap illuminated by:");
        dbgAppendMessage("1. The mesh itself (initially black)");
        dbgAppendMessage("2. A white sky (1.0f, 1.0f, 1.0f)");
	}
	void update() { renderLater(); }
	void render() {
		if (!m_device) m_device = new QOpenGLPaintDevice;
		m_device->setSize(size());
		QPainter painter(m_device);
		render(&painter);
	}
	void mousePressEvent(QMouseEvent *event) {
		m_cursor = QPoint(event->x(), event->y());
		Qt::KeyboardModifiers modifiers = event->modifiers();
		m_mouse_left = (event->buttons() & Qt::LeftButton);
	}
	void mouseReleaseEvent(QMouseEvent *event) {
		m_cursor = QPoint(event->x(), event->y());
		Qt::KeyboardModifiers modifiers = event->modifiers();
		m_mouse_left = !(event->button() == Qt::LeftButton);
	}
	void mouseMoveEvent(QMouseEvent *event) {
		m_cursor = QPoint(event->x(), event->y());
	}
	void keyPressEvent(QKeyEvent* event) {
		switch(event->key()) {
			case Qt::Key_Escape: close(); break;
            case Qt::Key_Space: bake(&m_scene); break;
			default: event->ignore();
			break;
		}
	}
	void quit() { m_done = true; }
	bool done() const { return m_done; }
protected:
	void closeEvent(QCloseEvent *event) { quit(); }
	bool event(QEvent *event) {
		switch (event->type()) {
			case QEvent::UpdateRequest:
				m_update_pending = false;
				renderNow();
				return true;
			default:
				return QWindow::event(event);
		}
	}
	void exposeEvent(QExposeEvent *event) {
		Q_UNUSED(event);
		if (isExposed()) renderNow();
	}
	void resizeEvent(QResizeEvent *event)
	{
        m_size = event->size();
		renderLater();
	}
public slots:
	void renderLater() {
		if (!m_update_pending) {
			m_update_pending = true;
			QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
		}
	}
	void renderNow() {
		if (!isExposed()) return;
		bool needsInitialize = false;
		if (!m_context) {
			m_context = new QOpenGLContext(this);
			m_context->setFormat(requestedFormat());
			m_context->create();
			needsInitialize = true;
		}
		m_context->makeCurrent(this);
		if (needsInitialize) {
			initializeOpenGLFunctions();
			initialize();
		}
		render();
		m_context->swapBuffers(this);
		if (m_auto_refresh) renderLater();
	}
};


// == Sproutline utilities ==

static FILE * svg_begin(const char *filename, int w, int h)
{
	FILE *f = fopen(filename, "w");
	if (!f) return 0;
	fprintf(f,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n"
		"<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
		"\txmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:ev=\"http://www.w3.org/2001/xml-events\"\n"
		"\tversion=\"1.1\" baseProfile=\"full\"\n"
		"\twidth=\"%d\" height=\"%d\"\n"
		"\tviewBox=\"0 0 %d %d\">\n\n",
		w, h, w, h);
	return f;
}

static void svg_tinted_background_image(FILE *f, const char *filename)
{
	if (!f) return;
	fprintf(f, "\t<image x=\"-0.5\" y=\"-0.5\" width=\"100%%\" height=\"100%%\" xlink:href=\"%s\" />\n", filename);
	fprintf(f, "\t<rect width=\"10000\" height=\"10000\" style=\"fill:rgb(0,255,0);stroke-width:0;\" fill-opacity=\"0.5\" />\n");
}

static void svg_polygon(FILE *f, const s2o_point *outline, int length)
{
	int i;
	if (!f) return;
	fprintf(f, "\t<polygon points=\"");
	for (i = 0; i < length; i++)
		fprintf(f, "%d,%d ", outline[i].x, outline[i].y);
	fprintf(f, "\" style=\"fill:none;stroke:red;stroke-width:1\" />\n");
}

static void svg_points(FILE *f, const s2o_point *outline, int length)
{
	int i;
	if (!f) return;
	for (i = 0; i < length; i++)
		fprintf(f, "\t<circle cx=\"%d\" cy=\"%d\" r=\"0.6\" />\n", outline[i].x, outline[i].y);
}

static void svg_end(FILE *f)
{
	if (!f) return;
	fprintf(f, "</svg>");
	fclose(f);
}

static int bake(scene_t *scene)
{
	lm_context *ctx = lmCreate(
		64,               // hemisphere resolution (power of two, max=512)
		0.001f, 100.0f,   // zNear, zFar of hemisphere cameras
		1.0f, 1.0f, 1.0f, // background color (white for ambient occlusion)
		2, 0.01f);        // lightmap interpolation threshold (small differences are interpolated rather than sampled)
	                      // check debug_interpolation.tga for an overview of sampled (red) vs interpolated (green) pixels.
	if (!ctx)
	{
		fprintf(stderr, "Error: Could not initialize lightmapper.\n");
		return 0;
	}
	
	int w = scene->w, h = scene->h;
	float *data = (float *)calloc(w * h * 4, sizeof(float));
	lmSetTargetLightmap(ctx, data, w, h, 4);

	lmSetGeometry(ctx, NULL,
		LM_FLOAT, (unsigned char*)scene->vertices + offsetof(vertex_t, p), sizeof(vertex_t),
		LM_FLOAT, (unsigned char*)scene->vertices + offsetof(vertex_t, t), sizeof(vertex_t),
		scene->indexCount, LM_UNSIGNED_SHORT, scene->indices);

	int vp[4];
	float view[16], projection[16];
	double lastUpdateTime = 0.0;
	while (lmBegin(ctx, vp, view, projection))
	{
		// render to lightmapper framebuffer
		glViewport(vp[0], vp[1], vp[2], vp[3]);
		drawScene(scene, view, projection);

		// display progress every second (printf is expensive)
		double time = qtGetTime();
		if (time - lastUpdateTime > 1.0)
		{
			lastUpdateTime = time;
			printf("\r%6.2f%%", lmProgress(ctx) * 100.0f);
			fflush(stdout);
		}

		lmEnd(ctx);
	}
	printf("\rFinished baking %d triangles.\n", scene->indexCount / 3);
	
	lmDestroy(ctx);

	// postprocess texture
	float *temp = (float *)calloc(w * h * 4, sizeof(float));
	lmImageSmooth(data, temp, w, h, 4);
	lmImageDilate(temp, data, w, h, 4);
	for (int i = 0; i < 16; i++)
	{
		lmImageDilate(data, temp, w, h, 4);
		lmImageDilate(temp, data, w, h, 4);
	}
	lmImagePower(data, w, h, 4, 1.0f / 2.2f, 0x7); // gamma correct color channels
	free(temp);

	// save result to a file
	if (lmImageSaveTGAf("result.tga", data, w, h, 4, 1.0f))
		printf("Saved result.tga\n");

	// upload result
	glBindTexture(GL_TEXTURE_2D, scene->lightmap);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_FLOAT, data);
	free(data);

	return 1;
}


// helpers ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int loadSimpleObjFile(const char *filename, vertex_t **vertices, unsigned int *vertexCount, unsigned short **indices, unsigned int *indexCount);
static GLuint loadProgram(const char *vp, const char *fp, const char **attributes, int attributeCount);

static int initScene(scene_t *scene)
{
	// load mesh
	if (!loadSimpleObjFile(":/gazebo.obj", &scene->vertices, &scene->vertexCount, &scene->indices, &scene->indexCount))
	{
		fprintf(stderr, "Error loading obj file\n");
		return 0;
	}

	glGenBuffers(1, &scene->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, scene->vbo);
	glBufferData(GL_ARRAY_BUFFER, scene->vertexCount * sizeof(vertex_t), scene->vertices, GL_STATIC_DRAW);

	glGenBuffers(1, &scene->ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene->ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, scene->indexCount * sizeof(unsigned short), scene->indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex_t), (void*)offsetof(vertex_t, p));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex_t), (void*)offsetof(vertex_t, t));

	// create lightmap texture
	scene->w = 654;
	scene->h = 654;
	glGenTextures(1, &scene->lightmap);
	glBindTexture(GL_TEXTURE_2D, scene->lightmap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	unsigned char emissive[] = { 0, 0, 0, 255 };
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, emissive);

	// load shader
	const char *vp =
		"#version 120\n"

		"attribute vec3 a_position;\n"
		"attribute vec2 a_texcoord;\n"
		"uniform mat4 u_view;\n"
		"uniform mat4 u_projection;\n"
		"varying vec2 v_texcoord;\n"

		"void main()\n"
		"{\n"
		"gl_Position = u_projection * (u_view * vec4(a_position, 1.0));\n"
		"v_texcoord = a_texcoord;\n"
		"}\n";

	const char *fp =
		"#version 120\n"

        "varying vec2 v_texcoord;\n"
		"uniform sampler2D u_lightmap;\n"

		"void main()\n"
		"{\n"
		"gl_FragColor = vec4(texture2D(u_lightmap, v_texcoord).rgb, gl_FrontFacing ? 1.0 : 0.0);\n"
		"}\n";

    const char *attribs[] =
    {
        "a_position",
        "a_texcoord"
    };
    
	scene->program = loadProgram(vp, fp, attribs, 2);
	if (!scene->program)
	{
		fprintf(stderr, "Error loading shader\n");
		return 0;
	}
	scene->u_view = glGetUniformLocation(scene->program, "u_view");
	scene->u_projection = glGetUniformLocation(scene->program, "u_projection");
	scene->u_lightmap = glGetUniformLocation(scene->program, "u_lightmap");

    return 1;
}

static void drawScene(scene_t *scene, float *view, float *projection)
{
	glEnable(GL_DEPTH_TEST);

    glUseProgram(scene->program);
	glUniform1i(scene->u_lightmap, 0);
	glUniformMatrix4fv(scene->u_projection, 1, GL_FALSE, projection);
	glUniformMatrix4fv(scene->u_view, 1, GL_FALSE, view);

	glBindTexture(GL_TEXTURE_2D, scene->lightmap);

    //! we are working with OpenGL 2.0
    
	glBindBuffer(GL_ARRAY_BUFFER, scene->vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene->ibo);
	glDrawElements(GL_TRIANGLES, scene->indexCount, GL_UNSIGNED_SHORT, 0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

static void destroyScene(scene_t *scene)
{
	free(scene->vertices);
	free(scene->indices);
	glDeleteBuffers(1, &scene->vbo);
	glDeleteBuffers(1, &scene->ibo);
	glDeleteTextures(1, &scene->lightmap);
	glDeleteProgram(scene->program);
}

static int loadSimpleObjFile(const char *filename, vertex_t **vertices, unsigned int *vertexCount, unsigned short **indices, unsigned int *indexCount)
{
	QFile file(filename);
	if (!file.open(QIODevice::ReadOnly))
		return 0;

    QTextStream in(&file);
    // first pass
	unsigned int np = 0, nn = 0, nt = 0, nf = 0;
    while(true)
	{
        QString line = in.readLine();
        if(line.isNull()) break;

		if (line[0] == '#') continue;
		if (line[0] == 'v')
		{
			if (line[1] == ' ') { np++; continue; }
			if (line[1] == 'n') { nn++; continue; }
			if (line[1] == 't') { nt++; continue; }
			assert(!"unknown vertex attribute");
		}
		if (line[0] == 'f') { nf++; continue; }
		assert(!"unknown identifier");
	};
	assert(np && np == nn && np == nt && nf); // only supports obj files without separately indexed vertex attributes

	// allocate memory
	*vertexCount = np;
	*vertices = (vertex_t *)calloc(np, sizeof(vertex_t));
	*indexCount = nf * 3;
	*indices = (unsigned short *)calloc(nf * 3, sizeof(unsigned short));

	// second pass
	file.seek(0);
	unsigned int cp = 0, cn = 0, ct = 0, cf = 0;
    while(true)
	{
        QString ln = in.readLine();
        if(ln.isNull()) break;
        
        char *line = ln.toLatin1().data();

		if (line[0] == '#') continue;
		if (line[0] == 'v')
		{
			if (line[1] == ' ') { float *p = (*vertices)[cp++].p; char *e1, *e2; p[0] = (float)strtod(line + 2, &e1); p[1] = (float)strtod(e1, &e2); p[2] = (float)strtod(e2, 0); continue; }
			if (line[1] == 'n') { /*float *n = (*vertices)[cn++].n; char *e1, *e2; n[0] = (float)strtod(line + 3, &e1); n[1] = (float)strtod(e1, &e2); n[2] = (float)strtod(e2, 0);*/ continue; } // no normals needed
			if (line[1] == 't') { float *t = (*vertices)[ct++].t; char *e1;      t[0] = (float)strtod(line + 3, &e1); t[1] = (float)strtod(e1, 0);                                continue; }
			assert(!"unknown vertex attribute");
		}
		if (line[0] == 'f')
		{
			unsigned short *tri = (*indices) + cf;
			cf += 3;
			char *e1, *e2, *e3 = line + 1;
			for (int i = 0; i < 3; i++)
			{
				unsigned long pi = strtoul(e3 + 1, &e1, 10);
				assert(e1[0] == '/');
				unsigned long ti = strtoul(e1 + 1, &e2, 10);
				assert(e2[0] == '/');
				unsigned long ni = strtoul(e2 + 1, &e3, 10);
				assert(pi == ti && pi == ni);
				tri[i] = (unsigned short)(pi - 1);
			}
			continue;
		}
		assert(!"unknown identifier");
	}

	return 1;
}


int main(int argc, char *argv[]) {

    QApplication app(argc, argv);
    app.setApplicationName("gamedevtool");
    app.setOrganizationName("KomSoft Oprogramowanie");
    app.setOrganizationDomain("komsoft.ath.cx");

	QSurfaceFormat surface_format = QSurfaceFormat::defaultFormat();
	surface_format.setAlphaBufferSize( 0 );
	surface_format.setDepthBufferSize( 0 );
	// surface_format.setRedBufferSize( 8 );
	// surface_format.setBlueBufferSize( 8 );
	// surface_format.setGreenBufferSize( 8 );
	// surface_format.setOption( QSurfaceFormat::DebugContext );
	// surface_format.setProfile( QSurfaceFormat::NoProfile );
	// surface_format.setRenderableType( QSurfaceFormat::OpenGLES );
	// surface_format.setSamples( 4 );
	// surface_format.setStencilBufferSize( 8 );
	// surface_format.setSwapBehavior( QSurfaceFormat::DefaultSwapBehavior );
	// surface_format.setSwapInterval( 1 );
	// surface_format.setVersion( 2, 0 );
	QSurfaceFormat::setDefaultFormat( surface_format );

    Window window;
    window.show();
	window.resize(800, 600);

    return app.exec();
}


static GLuint loadShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	if (shader == 0)
	{
		fprintf(stderr, "Could not create shader!\n");
		return 0;
	}
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint compiled;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled)
	{
		fprintf(stderr, "Could not compile shader!\n");
		GLint infoLen = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
		if (infoLen)
		{
			char* infoLog = (char*)malloc(infoLen);
			glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
			fprintf(stderr, "%s\n", infoLog);
			free(infoLog);
		}
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}
static GLuint loadProgram(const char *vp, const char *fp, const char **attributes, int attributeCount)
{
	GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vp);
	if (!vertexShader)
		return 0;
	GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fp);
	if (!fragmentShader)
	{
		glDeleteShader(vertexShader);
		return 0;
	}

	GLuint program = glCreateProgram();
	if (program == 0)
	{
		fprintf(stderr, "Could not create program!\n");
		return 0;
	}
	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);
    
    for (int i = 0; i < attributeCount; i++)
        glBindAttribLocation(program, i, attributes[i]);
    
	glLinkProgram(program);
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);
	GLint linked;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		fprintf(stderr, "Could not link program!\n");
		GLint infoLen = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
		if (infoLen)
		{
			char* infoLog = (char*)malloc(sizeof(char) * infoLen);
			glGetProgramInfoLog(program, infoLen, NULL, infoLog);
			fprintf(stderr, "%s\n", infoLog);
			free(infoLog);
		}
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static void multiplyMatrices(float *out, float *a, float *b)
{
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			out[y * 4 + x] = a[x] * b[y * 4] + a[4 + x] * b[y * 4 + 1] + a[8 + x] * b[y * 4 + 2] + a[12 + x] * b[y * 4 + 3];
}
static void translationMatrix(float *out, float x, float y, float z)
{
	out[ 0] = 1.0f; out[ 1] = 0.0f; out[ 2] = 0.0f; out[ 3] = 0.0f;
	out[ 4] = 0.0f; out[ 5] = 1.0f; out[ 6] = 0.0f; out[ 7] = 0.0f;
	out[ 8] = 0.0f; out[ 9] = 0.0f; out[10] = 1.0f; out[11] = 0.0f;
	out[12] = x;    out[13] = y;    out[14] = z;    out[15] = 1.0f;
}
static void rotationMatrix(float *out, float angle, float x, float y, float z)
{
	angle *= (float)M_PI / 180.0f;
	float c = cosf(angle), s = sinf(angle), c2 = 1.0f - c;
	out[ 0] = x*x*c2 + c;   out[ 1] = y*x*c2 + z*s; out[ 2] = x*z*c2 - y*s; out[ 3] = 0.0f;
	out[ 4] = x*y*c2 - z*s; out[ 5] = y*y*c2 + c;   out[ 6] = y*z*c2 + x*s; out[ 7] = 0.0f;
	out[ 8] = x*z*c2 + y*s; out[ 9] = y*z*c2 - x*s; out[10] = z*z*c2 + c;   out[11] = 0.0f;
	out[12] = 0.0f;         out[13] = 0.0f;         out[14] = 0.0f;         out[15] = 1.0f;
}
static void transformPosition(float *out, float *m, float *p)
{
	float d = 1.0f / (m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15]);
	out[2] =     d * (m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]);
	out[1] =     d * (m[1] * p[0] + m[5] * p[1] + m[ 9] * p[2] + m[13]);
	out[0] =     d * (m[0] * p[0] + m[4] * p[1] + m[ 8] * p[2] + m[12]);
}
static void transposeMatrix(float *out, float *m)
{
	out[ 0] = m[0]; out[ 1] = m[4]; out[ 2] = m[ 8]; out[ 3] = m[12];
	out[ 4] = m[1]; out[ 5] = m[5]; out[ 6] = m[ 9]; out[ 7] = m[13];
	out[ 8] = m[2]; out[ 9] = m[6]; out[10] = m[10]; out[11] = m[14];
	out[12] = m[3]; out[13] = m[7]; out[14] = m[11]; out[15] = m[15];
}
static void perspectiveMatrix(float *out, float fovy, float aspect, float zNear, float zFar)
{
	float f = 1.0f / tanf(fovy * (float)M_PI / 360.0f);
	float izFN = 1.0f / (zNear - zFar);
	out[ 0] = f / aspect; out[ 1] = 0.0f; out[ 2] = 0.0f;                       out[ 3] = 0.0f;
	out[ 4] = 0.0f;       out[ 5] = f;    out[ 6] = 0.0f;                       out[ 7] = 0.0f;
	out[ 8] = 0.0f;       out[ 9] = 0.0f; out[10] = (zFar + zNear) * izFN;      out[11] = -1.0f;
	out[12] = 0.0f;       out[13] = 0.0f; out[14] = 2.0f * zFar * zNear * izFN; out[15] = 0.0f;
}

#include "example.moc"

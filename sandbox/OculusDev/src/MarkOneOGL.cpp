#include <GL/glew.h>
#include <X11/X.h>
#include <X11/extensions/Xrandr.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <OVR.h>
#include <OVR_CAPI.h>
#include <OVR_CAPI_GL.h>

#include <vrpn_Tracker.h>
#include <vrpn_Button.h>
#include <vrpn_Analog.h>

#include <iostream>
#include <math.h>

#include <objloader.hpp>
#include <texture.hpp>
#include <shader.hpp>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <boost/thread/mutex.hpp>


// Oculus
ovrHmd hmd;
ovrHmdDesc hmd_desc;
ovrFovPort eye_fov[2];
ovrGLConfig ovr_gl_config;
ovrEyeRenderDesc eye_render_desc[2];

// OpenGL
std::vector<unsigned short> indices;
std::vector<glm::vec3> vertices;
std::vector<glm::vec2> uvs;
std::vector<glm::vec3> normals;

// GLFW
const bool fullscreen = true;
// const bool fullscreen = false;
GLFWwindow* window;

// VRPN
vrpn_Tracker_Remote* oculus_tracker;
vrpn_Tracker_Remote* tool_tracker;

// My System
struct Pose {
    float x;
    float y;
    float z;

    glm::quat orient;
};

boost::mutex pose_mutex;
Pose tool_pose;

// We are assuming that what the rigid body being tracked by the tracking
// system is identical to the view
boost::mutex tf_mutex;
glm::mat4 view_ovr;
glm::mat4 world_ovr;
glm::mat4 world_mask;
glm::mat4 world_model;

bool firstLocalization;
bool isVisible;


void VRPN_CALLBACK toolTrackerCallback(void* userData, const vrpn_TRACKERCB t)
{

    glm::mat4 wm;
    {
        boost::mutex::scoped_lock lock(tf_mutex);
        wm = world_mask;
    }

    glm::vec4 tool_position = 
        glm::inverse(wm) * 
        glm::vec4(
                (float) t.pos[0],
                (float) t.pos[1],
                (float) t.pos[2],
                1.0f
                );


    Pose tp;
    tp.x = tool_position[0];
    tp.y = tool_position[1];
    tp.z = tool_position[2];

    tp.orient = 
        glm::normalize(
                glm::quat_cast(glm::inverse(wm)) *
                glm::quat(
                    t.quat[3],
                    t.quat[0],
                    t.quat[1],
                    t.quat[2])
                );

    {
        boost::mutex::scoped_lock lock(pose_mutex);

        tool_pose.x = tp.x;
        tool_pose.y = tp.y;
        tool_pose.z = tp.z;
        tool_pose.orient = tp.orient;
    }

    std::cout << "Tool Orientation: " << 
        tp.orient.w << "," <<
        tp.orient.x << "," <<
        tp.orient.y << "," <<
        tp.orient.z << std::endl;

    // std::cout << "Tool Position: " << 
    //     tp.x << "," <<
    //     tp.y << "," <<
    //     tp.z << std::endl;


}

void VRPN_CALLBACK oculusTrackerCallback(void* userData, const vrpn_TRACKERCB t)
{
    /* 
     * We negate the x and z components because, this is the only way we can
     * report the orientation with respect to the world frame rotated 180deg.
     * Because rotations are not commutative we cannot calculate this 
     * analytically. There would exist one transformation between the world
     * frame and the reported orientation. 
     */

    glm::quat q_wm = 
        glm::normalize(
                glm::quat(
                    t.quat[3], 
                    -1.0f * t.quat[0], 
                    t.quat[1], 
                    -1.0f * t.quat[2])
                );

    {
        boost::mutex::scoped_lock lock(tf_mutex);
        world_mask = 
            glm::translate(
                    glm::mat4(1.0f), 
                    glm::vec3(t.pos[0], t.pos[1], t.pos[2])
                    ) *
            glm::mat4_cast(q_wm);
    }


    firstLocalization = false;
}

static void windowSizeCallback(GLFWwindow* p_Window, int p_Width, int p_Height)
{
    ovr_gl_config.OGL.Header.RTSize.w = p_Width; 
    ovr_gl_config.OGL.Header.RTSize.h = p_Height;

    int distortion_caps 
        = ovrDistortionCap_Chromatic | ovrDistortionCap_TimeWarp;

    ovrHmd_ConfigureRendering(hmd, 
            &ovr_gl_config.Config, 
            distortion_caps, 
            eye_fov, 
            eye_render_desc);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}


static void setOpenGLState(void)
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);

}

void initVrpn(void) 
{
    oculus_tracker = 
        new vrpn_Tracker_Remote("Oculus@158.130.62.126:3883");
    oculus_tracker->register_change_handler(0, oculusTrackerCallback);

    tool_tracker = 
        new vrpn_Tracker_Remote("Tool@158.130.62.126:3883");
    tool_tracker->register_change_handler(0, toolTrackerCallback);
}

void initOvr(void) 
{
    ovr_Initialize();
    hmd = ovrHmd_Create(0);
    if (!hmd) 
        hmd = ovrHmd_CreateDebug(ovrHmd_DK1);
    ovrHmd_GetDesc(hmd, &hmd_desc);

    // Start the sensor which provides the Rift’s pose and motion.
    ovrHmd_StartSensor(hmd, 
            ovrSensorCap_Orientation | 
            ovrSensorCap_YawCorrection | 
            ovrSensorCap_Position, 
            ovrSensorCap_Orientation);
}

glm::mat4 fromOVRMatrix4f(const OVR::Matrix4f &in) 
{
    glm::mat4 out;
    memcpy(glm::value_ptr(out), &in, sizeof(in));
    return out;
}

int main(void)
{

    initOvr();

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize glfw!\n");
        exit(EXIT_FAILURE);
    }

    ovrSizei client_size;

    if (fullscreen) {

        int count;
        GLFWmonitor** monitors = glfwGetMonitors(&count);

        client_size.w = hmd_desc.Resolution.w;
        client_size.h = hmd_desc.Resolution.h;

        window = glfwCreateWindow(
                client_size.w, 
                client_size.h, 
                "GLFW Oculus Rift Test", 
                monitors[1], 
                NULL);

    } else {

        client_size.w = 640;
        client_size.h = 480;

        window = glfwCreateWindow(client_size.w, 
                client_size.h, 
                "GLFW Oculus Rift Test", 
                NULL, 
                NULL);
    }


    if (!window) {
        glfwTerminate();
        exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;
    GLenum l_Result = glewInit();
    if (l_Result!=GLEW_OK) {
        printf("glewInit() error.\n");
        exit(EXIT_FAILURE);
    }

    setOpenGLState();

    /* 
     * Rendering to a framebuffer requires a bound texture, so we need
     * to get some of the parameters of the texture from OVR.
     */

    ovrSizei texture_size_left = ovrHmd_GetFovTextureSize(hmd, 
            ovrEye_Left, 
            hmd_desc.DefaultEyeFov[0], 
            1.0f);
    ovrSizei texture_size_right = ovrHmd_GetFovTextureSize(hmd, 
            ovrEye_Right, 
            hmd_desc.DefaultEyeFov[1], 
            1.0f);
    ovrSizei texture_size;
    texture_size.w = texture_size_left.w + texture_size_right.w;
    texture_size.h = (texture_size_left.h > texture_size_right.h \
            ? texture_size_left.h : texture_size_right.h);


    /*
     * Run of the mill framebuffer initialization
     */

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    GLuint color_texture_id;
    glGenTextures(1, &color_texture_id);
    glBindTexture(GL_TEXTURE_2D, color_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 
            0, 
            GL_RGBA, 
            texture_size.w, 
            texture_size.h, 
            0, 
            GL_RGBA, 
            GL_UNSIGNED_BYTE, 
            0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    GLuint depth_buffer_id;
    glGenRenderbuffers(1, &depth_buffer_id);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_buffer_id);
    glRenderbufferStorage(GL_RENDERBUFFER, 
            GL_DEPTH_COMPONENT, 
            texture_size.w, 
            texture_size.h);

    glFramebufferTexture(GL_FRAMEBUFFER, 
            GL_COLOR_ATTACHMENT0, 
            color_texture_id, 
            0);

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, 
            GL_DEPTH_ATTACHMENT, 
            GL_RENDERBUFFER, 
            depth_buffer_id);

    static const GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, draw_buffers);

    GLenum fbo_check = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (fbo_check != GL_FRAMEBUFFER_COMPLETE) {
        printf("There is a problem with the FBO.\n");
        exit(EXIT_FAILURE);
    }

    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);


    /*
     * Some oculus configurations
     */

    eye_fov[0] = hmd_desc.DefaultEyeFov[0];
    eye_fov[1] = hmd_desc.DefaultEyeFov[1];

    ovr_gl_config.OGL.Header.API = ovrRenderAPI_OpenGL;
    ovr_gl_config.OGL.Header.Multisample = 0;
    ovr_gl_config.OGL.Header.RTSize.w = client_size.w;
    ovr_gl_config.OGL.Header.RTSize.h = client_size.h;
    ovr_gl_config.OGL.Win = glfwGetX11Window(window);
    ovr_gl_config.OGL.Disp = glfwGetX11Display();

    int distortion_caps = ovrDistortionCap_Chromatic | 
        ovrDistortionCap_TimeWarp;

    ovrHmd_ConfigureRendering(hmd, 
            &ovr_gl_config.Config, 
            distortion_caps, 
            eye_fov, 
            eye_render_desc);

    ovrGLTexture eye_texture[2];
    eye_texture[0].OGL.Header.API = ovrRenderAPI_OpenGL;
    eye_texture[0].OGL.Header.TextureSize.w = texture_size.w;
    eye_texture[0].OGL.Header.TextureSize.h = texture_size.h;
    eye_texture[0].OGL.Header.RenderViewport.Pos.x = 0;
    eye_texture[0].OGL.Header.RenderViewport.Pos.y = 0;
    eye_texture[0].OGL.Header.RenderViewport.Size.w = texture_size.w/2;
    eye_texture[0].OGL.Header.RenderViewport.Size.h = texture_size.h;
    eye_texture[0].OGL.TexId = color_texture_id;
    // Right eye the same, except for the x-position in the texture...
    eye_texture[1] = eye_texture[0];
    eye_texture[1].OGL.Header.RenderViewport.Pos.x = (texture_size.w+1)/2;


    /*
     * Compilation of shaders and initialization of buffers
     */

    GLuint program_id = LoadShaders("../shaders/StandardShading.vertexshader",
            "../shaders/StandardShading.fragmentshader");

    bool res = loadOBJ("../assets/can.obj", vertices, uvs, normals);
    // bool res = loadAssImp("../assets/can.obj", indices, vertices, uvs, normals);

    GLuint vertex_array_id;
    glGenVertexArrays(1, &vertex_array_id);
    glBindVertexArray(vertex_array_id);

    GLuint vertex_buffer;
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER,
            vertices.size() * sizeof(glm::vec3),
            &vertices[0],
            GL_STATIC_DRAW);

    GLuint mvp_id = glGetUniformLocation(program_id, "MVP");

    glfwSetWindowSizeCallback(window, windowSizeCallback);

    /*
     * Initialization of local system variables
     */

    firstLocalization = true;
    isVisible = false;

    // glm::mat4 ovr_model =
    //     glm::lookAt(
    //         glm::vec3(0.0f, 0.0f, 4.0f),
    //         glm::vec3(0.0f, 0.0f, 0.0f),
    //         glm::vec3(0.0f, 1.0f, 0.0f)
    //         );
    view_ovr = glm::mat4(1.0f);
    world_ovr = glm::mat4_cast(glm::quat(0.0f, 0.0f, 1.0f, 0.0f));
    world_mask = glm::mat4(1.0f);
    world_model = glm::mat4(1.0f);

    glm::mat4 wm = world_mask;

    initVrpn();


    while (glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS && \
            !glfwWindowShouldClose(window))
    {
        oculus_tracker->mainloop();
        tool_tracker->mainloop();

        ovrFrameTiming m_HmdFrameTiming = ovrHmd_BeginFrame(hmd, 0);

        glUseProgram(program_id);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


        {
            boost::mutex::scoped_lock lock(tf_mutex);

            isVisible = 
                glm::any(
                        glm::notEqual(
                            glm::quat_cast(wm), 
                            glm::quat_cast(world_mask)
                            )
                        )
                ? true : false;

            wm = world_mask;
        }


        for (int eye_idx = 0; eye_idx < ovrEye_Count; eye_idx++)
        {
            ovrEyeType eye = hmd_desc.EyeRenderOrder[eye_idx];
            ovrPosef eye_pose = ovrHmd_BeginEyeRender(hmd, eye);

            glViewport(eye_texture[eye].OGL.Header.RenderViewport.Pos.x,
                    eye_texture[eye].OGL.Header.RenderViewport.Pos.y,
                    eye_texture[eye].OGL.Header.RenderViewport.Size.w,
                    eye_texture[eye].OGL.Header.RenderViewport.Size.h
                    );

            /*
             * Projection
             */ 

            glm::mat4 projection_matrix = 
                fromOVRMatrix4f(
                        OVR::Matrix4f(
                            ovrMatrix4f_Projection(
                                eye_render_desc[eye].Fov, 
                                0.3f, 
                                100.0f, 
                                true)).Transposed()
                        );


            /*
             * View
             */

            // Get data from the OVR
            glm::mat4 vo =
                fromOVRMatrix4f(
                        OVR::Matrix4f(
                            OVR::Quatf(eye_pose.Orientation)
                            )
                        );


            // Build the World -> OVR transform just once
            if (firstLocalization)
                world_ovr = wm * vo;


            glm::mat4 view_world;
            // Fuse the orientation data from both the optitrack and oculus
            if (isVisible) {
                glm::quat q_ovr = 
                    glm::normalize(
                            glm::quat_cast(vo * glm::inverse(world_ovr))
                            );
                glm::quat q_opt = 
                    glm::normalize(
                        glm::quat_cast(glm::inverse(wm))
                        );

                view_world = glm::mat4_cast(glm::mix(q_opt, q_ovr, 0.5f));
            } else {
                view_world = vo * glm::inverse(world_ovr);
            }

            // Extract translational component of the transformation
            glm::vec3 view_adjust = 
                glm::vec3(
                        eye_render_desc[eye].ViewAdjust.x,
                        eye_render_desc[eye].ViewAdjust.y,
                        eye_render_desc[eye].ViewAdjust.z
                        );

            glm::mat4 vo_t = 
                glm::translate(
                        glm::mat4(1.f),
                        glm::vec3((glm::inverse(wm))[3]) 
                        + view_adjust
                        );
            
            // Construct the Matrix
            glm::mat4 view_matrix = 
                vo_t *
                view_world *
                world_model;


            /*
             * Model
             */ 

            glm::mat4 model_matrix =
                glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f)) *
                glm::mat4_cast(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));


            /*
             * MVP
             */ 

            glm::mat4 mvp = 
                projection_matrix *
                view_matrix *
                model_matrix;


            glUniformMatrix4fv(mvp_id, 1, GL_FALSE, &mvp[0][0]);


            /*
             * Render...
             */

            glEnableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
            glDrawArrays(GL_TRIANGLES, 0, vertices.size());
            glDisableVertexAttribArray(0);

            ovrHmd_EndEyeRender(hmd, eye, eye_pose, &eye_texture[eye].Texture);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        ovrHmd_EndFrame(hmd);

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glfwPollEvents();
    }

    glDeleteBuffers(1, &vertex_buffer);
    glDeleteProgram(program_id);

    glfwDestroyWindow(window);
    glfwTerminate();

    ovrHmd_Destroy(hmd);
    ovr_Shutdown();

    exit(EXIT_SUCCESS);
}

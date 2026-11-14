// The plane of the eye of a view (game/EyePlane.h): a position is behind it
// exactly when its w of the clip space is negative, for each perspective
// projection, and no position is behind with a projection without
// perspective.

#include "EyePlane.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// A perspective projection of Direct3D (as CGraphics::SetProjection).
void Perspective(float scaleX, float scaleY, float nearPlane, float farPlane, float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = scaleX;
    m[5] = scaleY;
    m[10] = farPlane / (farPlane - nearPlane);
    m[11] = 1.0f;
    m[14] = -(nearPlane * farPlane) / (farPlane - nearPlane);
}

// The view of a camera at (px, py, pz) that looks along +x, with +y up: the
// view axes are right = -z, up = +y, forward = +x of the world.
void ViewAlongX(float px, float py, float pz, float m[16])
{
    const float rows[16] = {
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f,
        pz, -py, -px, 1.0f,
    };
    for (int i = 0; i < 16; i++)
        m[i] = rows[i];
}

int Fixed(double v)
{
    return (int)(v * 65536.0);
}

// The z and the w of the clip space of a position, in doubles.
void Clip(const float *view, const float *projection, double x, double y, double z, double *clipZ, double *clipW)
{
    double eye[4];
    for (int k = 0; k < 4; k++)
        eye[k] = x * view[k] + y * view[4 + k] + z * view[8 + k] + view[12 + k];
    *clipZ = eye[0] * projection[2] + eye[1] * projection[6] + eye[2] * projection[10] + eye[3] * projection[14];
    *clipW = eye[0] * projection[3] + eye[1] * projection[7] + eye[2] * projection[11] + eye[3] * projection[15];
}

} // namespace

int main()
{
    float view[16], projection[16];

    // The camera at (100, 5, -50) looks along +x.
    ViewAlongX(100.0f, 5.0f, -50.0f, view);
    Perspective(1.5f, 2.0f, 0.5f, 600.0f, projection);
    EyePlane eye = EyePlane_Of(view, projection);
    REQUIRE(!EyePlane_Behind(eye, Fixed(130.0), Fixed(5.0), Fixed(-50.0)));  // ahead
    REQUIRE(EyePlane_Behind(eye, Fixed(70.0), Fixed(5.0), Fixed(-50.0)));    // behind
    REQUIRE(!EyePlane_Behind(eye, Fixed(101.0), Fixed(5.0), Fixed(300.0)));  // ahead, far to the side
    REQUIRE(EyePlane_Behind(eye, Fixed(99.0), Fixed(400.0), Fixed(-50.0)));  // behind, far above

    // The same plane for another field of view and another far plane.
    float other[16];
    Perspective(0.7f, 0.9f, 0.1f, 150.0f, other);
    const EyePlane otherEye = EyePlane_Of(view, other);
    REQUIRE(otherEye.x == eye.x && otherEye.y == eye.y && otherEye.z == eye.z && otherEye.w == eye.w);

    // Against the clip space: behind is w < 0, and a position behind is
    // outside the near plane (z of the clip space < 0).
    unsigned seed = 12345;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (double)(seed >> 8) / (double)(1u << 24);
    };
    int behind = 0;
    for (int i = 0; i < 20000; i++) {
        const double x = 100.0 + (next() - 0.5) * 800.0;
        const double y = 5.0 + (next() - 0.5) * 100.0;
        const double z = -50.0 + (next() - 0.5) * 800.0;
        double clipZ, clipW;
        Clip(view, projection, x, y, z, &clipZ, &clipW);
        const bool is = EyePlane_Behind(eye, Fixed(x), Fixed(y), Fixed(z));
        if (clipW < -0.01)
            REQUIRE(is);
        if (clipW > 0.01)
            REQUIRE(!is);
        if (is) {
            REQUIRE(clipZ < 0.0);
            behind++;
        }
    }
    REQUIRE(behind > 5000 && behind < 15000);

    // Far from the origin (2,000): the rounding stays under 0.01.
    ViewAlongX(2000.0f, 30.0f, -1900.0f, view);
    eye = EyePlane_Of(view, projection);
    REQUIRE(!EyePlane_Behind(eye, Fixed(2000.01), Fixed(30.0), Fixed(-1900.0)));
    REQUIRE(EyePlane_Behind(eye, Fixed(1999.99), Fixed(30.0), Fixed(-1900.0)));

    // A projection without perspective: w is 1, no position is behind.
    float flat[16] = { 0 };
    flat[0] = 0.01f;
    flat[5] = 0.01f;
    flat[10] = 0.001f;
    flat[15] = 1.0f;
    eye = EyePlane_Of(view, flat);
    REQUIRE(!EyePlane_Behind(eye, Fixed(1500.0), Fixed(30.0), Fixed(-1900.0)));
    REQUIRE(!EyePlane_Behind(eye, Fixed(2500.0), Fixed(30.0), Fixed(-1900.0)));

    // A view that the integers of the plane cannot hold (a scale of 3): no
    // position is behind.
    ViewAlongX(100.0f, 5.0f, -50.0f, view);
    for (int i = 0; i < 12; i++)
        view[i] *= 3.0f;
    eye = EyePlane_Of(view, projection);
    REQUIRE(!EyePlane_Behind(eye, Fixed(20.0), Fixed(5.0), Fixed(-50.0)));
    REQUIRE(!EyePlane_Behind(eye, Fixed(130.0), Fixed(5.0), Fixed(-50.0)));

    printf("ok\n");
    return 0;
}

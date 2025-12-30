/*
 * RAY.C - Ray Tracer with Enhanced Reflections & Shadows
 * VGA Mode 0x13 (320x200, 256 colors)
 * Compile: tcc ray.c
 *
 * Controls:
 *   W/S     - Move forward/backward
 *   A/D     - Strafe left/right
 *   Q/E     - Move up/down
 *   Arrows  - Look around (rotate camera)
 *   1/2/3   - Quality settings
 *   ESC     - Exit
 */

#include <dos.h>
#include <math.h>
#include <conio.h>
#include <string.h>

#define W 320
#define H 200
#define NSPH 3
#define SHADES 16
#define FLOOR_Y -1.0f

#define PAL_RED    32
#define PAL_GREEN  48
#define PAL_BLUE   64
#define PAL_GRAY   80
#define PAL_WHITE  96

typedef struct { float x, y, z; } Vec3;
typedef struct { float r, g, b; } Color;

typedef struct {
    Vec3 cen;
    float rad, rad2, invrad;
    Color col;
    float refl;
    float spec_power;
} Sphere;

unsigned char far *vram = (unsigned char far *)0xA0000000L;
Sphere sph[NSPH];
Vec3 light;

Vec3 cam;
float cam_yaw = 0.0f;
float cam_pitch = 0.0f;

int quality = 2;
int max_depth = 3;

/*----- Vector Operations -----*/

float vdot(Vec3 a, Vec3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

Vec3 vadd(Vec3 a, Vec3 b) { 
    Vec3 r; 
    r.x = a.x + b.x; 
    r.y = a.y + b.y; 
    r.z = a.z + b.z; 
    return r; 
}

Vec3 vsub(Vec3 a, Vec3 b) { 
    Vec3 r; 
    r.x = a.x - b.x; 
    r.y = a.y - b.y; 
    r.z = a.z - b.z; 
    return r; 
}

Vec3 vscale(Vec3 v, float s) { 
    Vec3 r; 
    r.x = v.x * s; 
    r.y = v.y * s; 
    r.z = v.z * s; 
    return r; 
}

Vec3 vnorm(Vec3 v) {
    float len = sqrt(vdot(v, v));
    if (len > 0.0001f) { 
        float inv = 1.0f / len; 
        v.x *= inv; 
        v.y *= inv; 
        v.z *= inv; 
    }
    return v;
}

Vec3 vreflect(Vec3 v, Vec3 n) {
    float d = 2.0f * vdot(v, n);
    Vec3 r;
    r.x = v.x - d * n.x;
    r.y = v.y - d * n.y;
    r.z = v.z - d * n.z;
    return r;
}

/*----- Color Operations -----*/

Color cadd(Color a, Color b) {
    Color c;
    c.r = a.r + b.r;
    c.g = a.g + b.g;
    c.b = a.b + b.b;
    return c;
}

Color cscale(Color c, float s) {
    Color r;
    r.r = c.r * s;
    r.g = c.g * s;
    r.b = c.b * s;
    return r;
}

Color cmul(Color a, Color b) {
    Color c;
    c.r = a.r * b.r;
    c.g = a.g * b.g;
    c.b = a.b * b.b;
    return c;
}

Color clerp(Color a, Color b, float t) {
    Color c;
    c.r = a.r * (1.0f - t) + b.r * t;
    c.g = a.g * (1.0f - t) + b.g * t;
    c.b = a.b * (1.0f - t) + b.b * t;
    return c;
}

Color cclamp(Color c) {
    if (c.r > 1.0f) c.r = 1.0f;
    if (c.g > 1.0f) c.g = 1.0f;
    if (c.b > 1.0f) c.b = 1.0f;
    if (c.r < 0.0f) c.r = 0.0f;
    if (c.g < 0.0f) c.g = 0.0f;
    if (c.b < 0.0f) c.b = 0.0f;
    return c;
}

/*----- Video Functions -----*/

void set_mode(int m) {
    union REGS r;
    r.h.ah = 0;
    r.h.al = (unsigned char)m;
    int86(0x10, &r, &r);
}

void set_pal(int i, int r, int g, int b) {
    outportb(0x3C8, i);
    outportb(0x3C9, r);
    outportb(0x3C9, g);
    outportb(0x3C9, b);
}

void init_palette(void) {
    int i, v;
    
    set_pal(0, 0, 0, 0);
    
    /* Sky gradient */
    for (i = 1; i < 32; i++) {
        int b = 20 + i;
        set_pal(i, i/3, i/2 + 8, b > 63 ? 63 : b);
    }
    
    /* Red shades */
    for (i = 0; i < SHADES; i++) {
        v = 10 + i * 3;
        set_pal(PAL_RED + i, v > 63 ? 63 : v, i, i/2);
    }
    
    /* Green shades */
    for (i = 0; i < SHADES; i++) {
        v = 10 + i * 3;
        set_pal(PAL_GREEN + i, i/2, v > 63 ? 63 : v, i);
    }
    
    /* Blue shades */
    for (i = 0; i < SHADES; i++) {
        v = 12 + i * 3;
        set_pal(PAL_BLUE + i, i/2, i/2 + 6, v > 63 ? 63 : v);
    }
    
    /* Gray floor shades */
    for (i = 0; i < SHADES; i++) {
        v = 4 + i * 3;
        set_pal(PAL_GRAY + i, v, v + 2, v + 4);
    }
    
    /* White/specular highlights */
    for (i = 0; i < SHADES; i++) {
        v = 32 + i * 2;
        set_pal(PAL_WHITE + i, v > 63 ? 63 : v, v > 63 ? 63 : v, v > 63 ? 63 : v);
    }
}

void putpix(int x, int y, unsigned char c) {
    vram[(long)y * W + x] = c;
}

/*----- Intersection Tests -----*/

float hit_sphere(Vec3 o, Vec3 d, Sphere *s) {
    Vec3 oc;
    float b, c, disc, sq, t;
    
    oc.x = o.x - s->cen.x;
    oc.y = o.y - s->cen.y;
    oc.z = o.z - s->cen.z;
    
    b = vdot(oc, d);
    c = vdot(oc, oc) - s->rad2;
    disc = b*b - c;
    
    if (disc < 0.0f) return -1.0f;
    
    sq = sqrt(disc);
    t = -b - sq;
    if (t > 0.001f) return t;
    t = -b + sq;
    if (t > 0.001f) return t;
    
    return -1.0f;
}

float hit_floor(Vec3 o, Vec3 d) {
    float t;
    if (d.y >= 0.0f) return -1.0f;
    t = (FLOOR_Y - o.y) / d.y;
    if (t > 0.001f && t < 100.0f) return t;
    return -1.0f;
}

/*----- Shadow Ray -----*/

float calc_shadow(Vec3 p, int skip_id) {
    Vec3 ldir;
    float ldist, t;
    int i;
    
    ldir = vsub(light, p);
    ldist = sqrt(vdot(ldir, ldir));
    ldir = vscale(ldir, 1.0f / ldist);
    
    for (i = 0; i < NSPH; i++) {
        if (i == skip_id) continue;
        t = hit_sphere(p, ldir, &sph[i]);
        if (t > 0.01f && t < ldist) {
            /* Partial shadow based on distance */
            return 0.15f;
        }
    }
    return 1.0f;
}

/*----- Main Ray Tracing -----*/

Color trace_ray(Vec3 o, Vec3 d, int depth);

Color shade_sphere(Vec3 o, Vec3 d, float t, int id, int depth) {
    Sphere *sp = &sph[id];
    Vec3 hit, norm, ldir, half, refl_dir;
    Color diffuse, specular, result, refl_col;
    float diff, spec, shadow, fresnel, ndotv;
    
    /* Hit point and normal */
    hit = vadd(o, vscale(d, t));
    norm.x = (hit.x - sp->cen.x) * sp->invrad;
    norm.y = (hit.y - sp->cen.y) * sp->invrad;
    norm.z = (hit.z - sp->cen.z) * sp->invrad;
    
    /* Light direction */
    ldir = vnorm(vsub(light, hit));
    
    /* Diffuse lighting */
    diff = vdot(norm, ldir);
    if (diff < 0.0f) diff = 0.0f;
    
    /* Specular (Blinn-Phong) */
    half = vnorm(vsub(ldir, d));
    spec = vdot(norm, half);
    if (spec < 0.0f) spec = 0.0f;
    
    /* Sharp specular power */
    spec = pow(spec, sp->spec_power);
    
    /* Shadow check */
    shadow = calc_shadow(hit, id);
    diff *= shadow;
    if (shadow < 0.5f) spec *= 0.1f;
    
    /* Base color with ambient + diffuse */
    diffuse = cscale(sp->col, 0.12f + 0.68f * diff);
    
    /* Add specular highlight (white) */
    specular.r = specular.g = specular.b = spec * 0.8f;
    result = cadd(diffuse, specular);
    
    /* Fresnel effect - more reflection at grazing angles */
    ndotv = -vdot(norm, d);
    if (ndotv < 0.0f) ndotv = 0.0f;
    fresnel = sp->refl + (1.0f - sp->refl) * pow(1.0f - ndotv, 3.0f);
    if (fresnel > 0.95f) fresnel = 0.95f;
    
    /* Reflection */
    if (depth > 0 && fresnel > 0.01f) {
        refl_dir = vreflect(d, norm);
        refl_col = trace_ray(hit, refl_dir, depth - 1);
        result = clerp(result, refl_col, fresnel);
    }
    
    return cclamp(result);
}

Color shade_floor(Vec3 o, Vec3 d, float t, int depth) {
    /* Все объявления переменных В НАЧАЛЕ функции */
    Vec3 hit, norm, ldir, refl_dir;
    Color base, result, refl_col, sky;
    int cx, cz, check;
    float diff, shadow, fresnel, fade;
    
    /* Инициализация */
    hit = vadd(o, vscale(d, t));
    norm.x = 0.0f; norm.y = 1.0f; norm.z = 0.0f;
    
    /* Checkerboard pattern */
    cx = (int)floor(hit.x);
    cz = (int)floor(hit.z);
    if (hit.x < 0) cx--;
    if (hit.z < 0) cz--;
    check = (cx + cz) & 1;
    
    if (check) {
        base.r = 0.7f; base.g = 0.7f; base.b = 0.75f;
    } else {
        base.r = 0.2f; base.g = 0.2f; base.b = 0.25f;
    }
    
    /* Direct lighting */
    ldir = vnorm(vsub(light, hit));
    diff = vdot(norm, ldir);
    if (diff < 0.0f) diff = 0.0f;
    
    /* Shadow from spheres */
    shadow = calc_shadow(hit, -1);
    diff *= shadow;
    
    result = cscale(base, 0.25f + 0.75f * diff * shadow);
    
    /* Reflective floor with Fresnel */
    if (depth > 0) {
        fresnel = 0.15f + 0.35f * pow(1.0f + vdot(d, norm), 2.0f);
        if (fresnel > 0.5f) fresnel = 0.5f;
        
        refl_dir = vreflect(d, norm);
        refl_col = trace_ray(hit, refl_dir, depth - 1);
        result = clerp(result, refl_col, fresnel);
    }
    
    /* Distance fade */
    if (t > 20.0f) {
        fade = (t - 20.0f) / 30.0f;
        if (fade > 1.0f) fade = 1.0f;
        sky.r = 0.4f; sky.g = 0.5f; sky.b = 0.7f;
        result = clerp(result, sky, fade);
    }
    
    return cclamp(result);
}

Color trace_ray(Vec3 o, Vec3 d, int depth) {
    float closest = 1e10f;
    float t;
    int hit_id = -1;
    int is_floor = 0;
    int i;
    Color sky;
    float grad;
    
    /* Find closest intersection */
    for (i = 0; i < NSPH; i++) {
        t = hit_sphere(o, d, &sph[i]);
        if (t > 0.0f && t < closest) { 
            closest = t; 
            hit_id = i; 
        }
    }
    
    t = hit_floor(o, d);
    if (t > 0.0f && t < closest) { 
        closest = t; 
        hit_id = -1; 
        is_floor = 1; 
    }
    
    /* Shade based on what we hit */
    if (is_floor) {
        return shade_floor(o, d, closest, depth);
    }
    
    if (hit_id >= 0) {
        return shade_sphere(o, d, closest, hit_id, depth);
    }
    
    /* Sky gradient */
    grad = 0.5f + 0.5f * d.y;
    sky.r = 0.3f + 0.2f * (1.0f - grad);
    sky.g = 0.4f + 0.3f * (1.0f - grad);
    sky.b = 0.6f + 0.35f * grad;
    return sky;
}

/*----- Color to Palette -----*/

unsigned char color_to_palette(Color c, int hit_type) {
    float lum;
    int shade;
    unsigned char base;
    
    c = cclamp(c);
    lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    
    /* Determine dominant color channel */
    if (hit_type == 0) {
        /* Red sphere */
        base = PAL_RED;
        lum = c.r * 0.6f + lum * 0.4f;
    } else if (hit_type == 1) {
        /* Green sphere */
        base = PAL_GREEN;
        lum = c.g * 0.6f + lum * 0.4f;
    } else if (hit_type == 2) {
        /* Blue sphere */
        base = PAL_BLUE;
        lum = c.b * 0.6f + lum * 0.4f;
    } else if (hit_type == 3) {
        /* Floor */
        base = PAL_GRAY;
    } else {
        /* Sky */
        shade = (int)(lum * 30.0f) + 1;
        if (shade < 1) shade = 1;
        if (shade > 31) shade = 31;
        return (unsigned char)shade;
    }
    
    shade = (int)(lum * (SHADES - 1));
    if (shade >= SHADES) shade = SHADES - 1;
    if (shade < 0) shade = 0;
    
    return base + (unsigned char)shade;
}

unsigned char trace(Vec3 o, Vec3 d, int depth) {
    float closest = 1e10f;
    float t;
    int hit_id = -1;
    int is_floor = 0;
    int i;
    Color c;
    
    /* Determine hit type for palette selection */
    for (i = 0; i < NSPH; i++) {
        t = hit_sphere(o, d, &sph[i]);
        if (t > 0.0f && t < closest) { 
            closest = t; 
            hit_id = i; 
        }
    }
    
    t = hit_floor(o, d);
    if (t > 0.0f && t < closest) { 
        is_floor = 1;
        hit_id = -1;
    }
    
    c = trace_ray(o, d, depth);
    
    if (is_floor) {
        return color_to_palette(c, 3);
    }
    if (hit_id >= 0) {
        return color_to_palette(c, hit_id);
    }
    return color_to_palette(c, 4);
}

/*----- Camera -----*/

Vec3 get_ray_dir(float dx, float dy) {
    Vec3 dir;
    float cosy, siny, cosp, sinp;
    float rx, ry, rz;
    
    dir.x = dx;
    dir.y = dy;
    dir.z = 1.0f;
    
    cosy = cos(cam_yaw);
    siny = sin(cam_yaw);
    cosp = cos(cam_pitch);
    sinp = sin(cam_pitch);
    
    rx = dir.x * cosy + dir.z * siny;
    rz = -dir.x * siny + dir.z * cosy;
    ry = dir.y * cosp - rz * sinp;
    rz = dir.y * sinp + rz * cosp;
    
    dir.x = rx;
    dir.y = ry;
    dir.z = rz;
    
    return vnorm(dir);
}

void render_frame(void) {
    float invW, invH, aspect, fov;
    int x, y, step;
    float dx, dy;
    Vec3 dir;
    unsigned char c;
    
    invW = 1.0f / (float)W;
    invH = 1.0f / (float)H;
    aspect = (float)W / (float)H;
    fov = 0.5773503f;
    
    step = quality;
    
    for (y = 0; y < H; y += step) {
        dy = (1.0f - 2.0f * y * invH) * fov;
        
        for (x = 0; x < W; x += step) {
            dx = (2.0f * x * invW - 1.0f) * aspect * fov;
            dir = get_ray_dir(dx, dy);
            c = trace(cam, dir, max_depth);
            
            if (step == 1) {
                putpix(x, y, c);
            } else if (step == 2) {
                putpix(x, y, c);
                putpix(x+1, y, c);
                putpix(x, y+1, c);
                putpix(x+1, y+1, c);
            } else {
                int bx, by;
                for (by = 0; by < step && y+by < H; by++) {
                    for (bx = 0; bx < step && x+bx < W; bx++) {
                        putpix(x+bx, y+by, c);
                    }
                }
            }
        }
    }
}

/*----- Movement -----*/

void move_forward(float dist) {
    cam.x += sin(cam_yaw) * cos(cam_pitch) * dist;
    cam.y += sin(cam_pitch) * dist;
    cam.z += cos(cam_yaw) * cos(cam_pitch) * dist;
}

void move_strafe(float dist) {
    cam.x += cos(cam_yaw) * dist;
    cam.z += -sin(cam_yaw) * dist;
}

void move_vertical(float dist) {
    cam.y += dist;
}

/*----- Main -----*/

int main(void) {
    int running = 1;
    int key;
    float move_speed = 0.4f;
    float rot_speed = 0.08f;
    
    cam.x = 0.0f;
    cam.y = 0.5f;
    cam.z = -3.0f;
    
    /* Red sphere - chrome-like */
    sph[0].cen.x = 0.0f;
    sph[0].cen.y = 0.0f;
    sph[0].cen.z = 5.0f;
    sph[0].rad = 1.0f;
    sph[0].rad2 = 1.0f;
    sph[0].invrad = 1.0f;
    sph[0].col.r = 0.9f; sph[0].col.g = 0.2f; sph[0].col.b = 0.15f;
    sph[0].refl = 0.65f;
    sph[0].spec_power = 64.0f;
    
    /* Green sphere - glossy */
    sph[1].cen.x = -2.5f;
    sph[1].cen.y = 0.5f;
    sph[1].cen.z = 7.0f;
    sph[1].rad = 1.5f;
    sph[1].rad2 = 2.25f;
    sph[1].invrad = 0.6667f;
    sph[1].col.r = 0.15f; sph[1].col.g = 0.85f; sph[1].col.b = 0.25f;
    sph[1].refl = 0.55f;
    sph[1].spec_power = 48.0f;
    
    /* Blue sphere - mirror-like */
    sph[2].cen.x = 1.8f;
    sph[2].cen.y = -0.3f;
    sph[2].cen.z = 3.5f;
    sph[2].rad = 0.7f;
    sph[2].rad2 = 0.49f;
    sph[2].invrad = 1.4286f;
    sph[2].col.r = 0.2f; sph[2].col.g = 0.35f; sph[2].col.b = 0.95f;
    sph[2].refl = 0.75f;
    sph[2].spec_power = 96.0f;
    
    light.x = 5.0f;
    light.y = 8.0f;
    light.z = -2.0f;
    
    set_mode(0x13);
    init_palette();
    
    while (running) {
        render_frame();
        
        while (kbhit()) {
            key = getch();
            
            if (key == 0 || key == 0xE0) {
                key = getch();
                switch (key) {
                    case 72: cam_pitch += rot_speed;
                        if (cam_pitch > 1.3f) cam_pitch = 1.3f; break;
                    case 80: cam_pitch -= rot_speed;
                        if (cam_pitch < -1.3f) cam_pitch = -1.3f; break;
                    case 75: cam_yaw -= rot_speed; break;
                    case 77: cam_yaw += rot_speed; break;
                }
            } else {
                switch (key) {
                    case 27: running = 0; break;
                    case 'w': case 'W': move_forward(move_speed); break;
                    case 's': case 'S': move_forward(-move_speed); break;
                    case 'a': case 'A': move_strafe(-move_speed); break;
                    case 'd': case 'D': move_strafe(move_speed); break;
                    case 'q': case 'Q': move_vertical(move_speed); break;
                    case 'e': case 'E': move_vertical(-move_speed); break;
                    case '1': quality = 4; max_depth = 2; break;
                    case '2': quality = 2; max_depth = 3; break;
                    case '3': quality = 1; max_depth = 4; break;
                }
            }
        }
    }
    
    set_mode(0x03);
    return 0;
}
/*
 * RAY.C - Ray Tracer with Real-Time Camera Movement in MS-DOS
 * VGA Mode 0x13 (320x200, 256 colors)
 * Compile: tcc ray.c
 * Requires: Pentium III
 *
 * Controls:
 *   W/S     - Move forward/backward
 *   A/D     - Strafe left/right
 *   Q/E     - Move up/down
 *   Arrows  - Look around (rotate camera)
 *   1/2     - Quality: 1=fast draft, 2=full quality
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

/*----- Structures -----*/

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3 cen;
    float rad, rad2, invrad;
    unsigned char base;
    float refl;
} Sphere;

unsigned char far *vram = (unsigned char far *)0xA0000000L;
Sphere sph[NSPH];
Vec3 light;

/* Camera state */
Vec3 cam;
float cam_yaw = 0.0f;    /* Rotation around Y (left/right) */
float cam_pitch = 0.0f;  /* Rotation around X (up/down) */

/* Rendering quality: 1=every pixel, 2=every 2nd, 4=every 4th */
int quality = 2;
int max_depth = 2;

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
    
    for (i = 1; i < 32; i++) {
        int b = 20 + i;
        set_pal(i, i/3, i/2 + 8, b > 63 ? 63 : b);
    }
    
    for (i = 0; i < SHADES; i++) {
        v = 12 + i * 3;
        set_pal(PAL_RED + i, v > 63 ? 63 : v, i/2, i/3);
        set_pal(PAL_GREEN + i, i/3, v > 63 ? 63 : v, i/2);
        set_pal(PAL_BLUE + i, i/2, i/2 + 4, v > 63 ? 63 : v);
    }
    
    for (i = 0; i < SHADES; i++) {
        v = 4 + i * 3;
        set_pal(PAL_GRAY + i, v, v + 2, v + 6);
    }
}

void putpix(int x, int y, unsigned char c) {
    vram[(long)y * W + x] = c;
}

void clear_screen(void) {
    _fmemset(vram, 0, (unsigned long)W * H);
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

int in_shadow(Vec3 p, Vec3 ldir, int skip) {
    int i;
    float t;
    for (i = 0; i < NSPH; i++) {
        if (i == skip) continue;
        t = hit_sphere(p, ldir, &sph[i]);
        if (t > 0.01f) return 1;
    }
    return 0;
}

/*----- Ray Tracing -----*/

unsigned char trace(Vec3 o, Vec3 d, int depth) {
    float closest, t;
    int hit_id, is_floor, i;
    Vec3 hit, norm, ldir, half, rd;
    float diff, spec, inten, fade;
    int shade, cx, cz, check;
    Sphere *sp;
    unsigned char rc;
    
    closest = 1e10f;
    hit_id = -1;
    is_floor = 0;
    
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
    
    if (is_floor) {
        hit = vadd(o, vscale(d, closest));
        norm.x = 0.0f; 
        norm.y = 1.0f; 
        norm.z = 0.0f;
        
        cx = (int)floor(hit.x);
        cz = (int)floor(hit.z);
        check = (cx + cz) & 1;
        
        if (depth > 0) {
            rd = vreflect(d, norm);
            rc = trace(hit, rd, depth - 1);
            
            fade = 1.0f - closest / 35.0f;
            if (fade < 0.15f) fade = 0.15f;
            
            shade = (int)((rc & 0x0F) * fade * 0.75f);
            if (check) shade += 4;
            if (shade >= SHADES) shade = SHADES - 1;
            if (shade < 0) shade = 0;
            
            return PAL_GRAY + (unsigned char)shade;
        }
        
        return PAL_GRAY + (check ? 10 : 4);
    }
    
    if (hit_id >= 0) {
        sp = &sph[hit_id];
        hit = vadd(o, vscale(d, closest));
        
        norm.x = (hit.x - sp->cen.x) * sp->invrad;
        norm.y = (hit.y - sp->cen.y) * sp->invrad;
        norm.z = (hit.z - sp->cen.z) * sp->invrad;
        
        ldir = vnorm(vsub(light, hit));
        
        diff = vdot(norm, ldir);
        if (diff < 0.0f) diff = 0.0f;
        
        half = vsub(ldir, d);
        half = vnorm(half);
        spec = vdot(norm, half);
        if (spec < 0.0f) spec = 0.0f;
        spec = spec * spec;
        spec = spec * spec;
        spec = spec * spec;
        spec = spec * spec;
        
        if (in_shadow(hit, ldir, hit_id)) {
            diff *= 0.15f;
            spec = 0.0f;
        }
        
        inten = 0.08f + 0.62f * diff + 0.55f * spec;
        
        if (depth > 0 && sp->refl > 0.0f) {
            rd = vreflect(d, norm);
            rc = trace(hit, rd, depth - 1);
            inten += sp->refl * (float)(rc & 0x0F) / (SHADES * 1.5f);
        }
        
        if (inten > 1.0f) inten = 1.0f;
        shade = (int)(inten * (SHADES - 1));
        if (shade >= SHADES) shade = SHADES - 1;
        
        return sp->base + (unsigned char)shade;
    }
    
    shade = (int)((1.0f - d.y * 0.5f - 0.5f) * 30.0f) + 1;
    if (shade < 1) shade = 1;
    if (shade > 31) shade = 31;
    return (unsigned char)shade;
}

/*----- Camera Direction Calculation -----*/

Vec3 get_ray_dir(float dx, float dy) {
    Vec3 dir;
    float cosy, siny, cosp, sinp;
    float rx, ry, rz, tx;
    
    /* Start with screen-space direction */
    dir.x = dx;
    dir.y = dy;
    dir.z = 1.0f;
    
    /* Precompute trig */
    cosy = cos(cam_yaw);
    siny = sin(cam_yaw);
    cosp = cos(cam_pitch);
    sinp = sin(cam_pitch);
    
    /* Rotate around Y axis (yaw) */
    rx = dir.x * cosy + dir.z * siny;
    rz = -dir.x * siny + dir.z * cosy;
    
    /* Rotate around X axis (pitch) */
    ry = dir.y * cosp - rz * sinp;
    rz = dir.y * sinp + rz * cosp;
    
    dir.x = rx;
    dir.y = ry;
    dir.z = rz;
    
    return vnorm(dir);
}

/*----- Render Frame -----*/

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
            
            /* Fill block for lower quality modes */
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

/*----- Movement Helpers -----*/

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

/*----- Main Program -----*/

int main(void) {
    int running = 1;
    int key;
    float move_speed = 0.4f;
    float rot_speed = 0.08f;
    
    /* Camera starting position */
    cam.x = 0.0f;
    cam.y = 0.5f;
    cam.z = -3.0f;
    cam_yaw = 0.0f;
    cam_pitch = 0.0f;
    
    /* Red sphere */
    sph[0].cen.x = 0.0f;
    sph[0].cen.y = 0.0f;
    sph[0].cen.z = 5.0f;
    sph[0].rad = 1.0f;
    sph[0].rad2 = 1.0f;
    sph[0].invrad = 1.0f;
    sph[0].base = PAL_RED;
    sph[0].refl = 0.25f;
    
    /* Green sphere */
    sph[1].cen.x = -2.5f;
    sph[1].cen.y = 0.5f;
    sph[1].cen.z = 7.0f;
    sph[1].rad = 1.5f;
    sph[1].rad2 = 2.25f;
    sph[1].invrad = 0.6667f;
    sph[1].base = PAL_GREEN;
    sph[1].refl = 0.2f;
    
    /* Blue sphere */
    sph[2].cen.x = 1.8f;
    sph[2].cen.y = -0.3f;
    sph[2].cen.z = 3.5f;
    sph[2].rad = 0.7f;
    sph[2].rad2 = 0.49f;
    sph[2].invrad = 1.4286f;
    sph[2].base = PAL_BLUE;
    sph[2].refl = 0.35f;
    
    /* Light source */
    light.x = 5.0f;
    light.y = 8.0f;
    light.z = -2.0f;
    
    /* Initialize graphics */
    set_mode(0x13);
    init_palette();
    
    /* Main loop */
    while (running) {
        render_frame();
        
        /* Process all pending keys */
        while (kbhit()) {
            key = getch();
            
            /* Extended key (arrows) */
            if (key == 0 || key == 0xE0) {
                key = getch();
                switch (key) {
                    case 72: /* Up arrow */
                        cam_pitch += rot_speed;
                        if (cam_pitch > 1.3f) cam_pitch = 1.3f;
                        break;
                    case 80: /* Down arrow */
                        cam_pitch -= rot_speed;
                        if (cam_pitch < -1.3f) cam_pitch = -1.3f;
                        break;
                    case 75: /* Left arrow */
                        cam_yaw -= rot_speed;
                        break;
                    case 77: /* Right arrow */
                        cam_yaw += rot_speed;
                        break;
                }
            } else {
                switch (key) {
                    case 27: /* ESC */
                        running = 0;
                        break;
                    
                    /* Movement */
                    case 'w': case 'W':
                        move_forward(move_speed);
                        break;
                    case 's': case 'S':
                        move_forward(-move_speed);
                        break;
                    case 'a': case 'A':
                        move_strafe(-move_speed);
                        break;
                    case 'd': case 'D':
                        move_strafe(move_speed);
                        break;
                    case 'q': case 'Q':
                        move_vertical(move_speed);
                        break;
                    case 'e': case 'E':
                        move_vertical(-move_speed);
                        break;
                    
                    /* Quality settings */
                    case '1':
                        quality = 4;  /* Fast: 4x4 blocks */
                        max_depth = 1;
                        break;
                    case '2':
                        quality = 2;  /* Medium: 2x2 blocks */
                        max_depth = 2;
                        break;
                    case '3':
                        quality = 1;  /* Full: every pixel */
                        max_depth = 2;
                        break;
                    
                    /* Speed adjustment */
                    case '+': case '=':
                        move_speed += 0.1f;
                        break;
                    case '-': case '_':
                        move_speed -= 0.1f;
                        if (move_speed < 0.1f) move_speed = 0.1f;
                        break;
                }
            }
        }
    }
    
    set_mode(0x03);
    
    return 0;
}
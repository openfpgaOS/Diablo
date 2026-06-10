//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * cxxdemo — C++ demo for openfpgaOS
 *
 * Demonstrates C++ features on the Pocket:
 *   - Classes and methods
 *   - Operator new / delete (heap allocation)
 *   - Static constructors / destructors
 *   - Templates
 *
 * Note: openfpgaOS bundles musl (a C library only) -- there is no
 * libstdc++. Use printf for output instead of std::cout. C++ language
 * features all work; only the C++ standard library is unavailable.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Static constructor test ──────────────────────────────────── */

static int init_counter = 0;

struct InitTracker {
    InitTracker() { init_counter++; }
};

static InitTracker tracker;  /* static constructor increments counter */

/* ── Simple class with virtual method ─────────────────────────── */

class Shape {
public:
    virtual ~Shape() {}
    virtual int area() const = 0;
    virtual const char *name() const = 0;
};

class Rectangle : public Shape {
    int w, h;
public:
    Rectangle(int w, int h) : w(w), h(h) {}
    int area() const override { return w * h; }
    const char *name() const override { return "Rectangle"; }
};

class Circle : public Shape {
    int r;
public:
    Circle(int r) : r(r) {}
    int area() const override { return (314 * r * r) / 100; } /* approx pi */
    const char *name() const override { return "Circle"; }
};

/* ── Template example ─────────────────────────────────────────── */

template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    //of_video_init();
    //of_print_clear();

    printf("=== openfpgaOS C++ Demo ===\n\n");

    /* Static constructor test */
    printf("Static constructors: %s (counter=%d)\n\n",
           init_counter > 0 ? "OK" : "FAIL", init_counter);

    /* Heap allocation with new/delete */
    Rectangle *rect = new Rectangle(10, 5);
    Circle *circ = new Circle(7);

    printf("Heap objects (new/delete):\n");
    printf("  %s area = %d\n", rect->name(), rect->area());
    printf("  %s area = %d\n\n", circ->name(), circ->area());

    delete rect;
    delete circ;
    printf("  delete: OK\n\n");

    /* Template instantiation */
    int   mi = max_of(42, 17);
    float mf = max_of(3.14f, 2.71f);
    printf("Templates:\n");
    printf("  max_of(42, 17)       = %d\n", mi);
    printf("  max_of(3.14, 2.71)   = %d.%02d\n\n",
           (int)mf, (int)(mf * 100) % 100);

    /* Array new/delete */
    int *arr = new int[5];
    for (int i = 0; i < 5; i++) arr[i] = i * i;
    printf("Array new[5]: ");
    for (int i = 0; i < 5; i++) printf("%d ", arr[i]);
    printf("\n");
    delete[] arr;
    printf("  delete[]: OK\n\n");

    printf("All C++ features working!\n");

    /* No <iostream> on openfpgaOS (no libstdc++ in the SDK).
     * Use printf from <stdio.h> for formatted output. */

    printf("Press START to exit.\n");

    /* Simple event loop */
    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_START))
            break;
        usleep(16 * 1000);
    }

    return 0;
}

#include <stdio.h>
#include <math.h>
#include <stdint.h>

// Map dimensions and data
#define MAP_WIDTH 24
#define MAP_HEIGHT 24

int worldMap[MAP_WIDTH][MAP_HEIGHT] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

// Screen dimensions
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

// Player position and direction
typedef struct {
    int x, y; // Player's position
    int dx, dy; // Player's direction vector
} Player;

int abs (int n) { return n > 0 ? n : -n; }

// Function to cast rays and render the scene
void render(Player player) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        // Calculate ray position and direction
        int cameraX = 2 * x / (double)SCREEN_WIDTH - 1; // x-coordinate in camera space
        int rayPosX = player.x;
        int rayPosY = player.y;
        int rayDirX = player.dx + player.dy * cameraX;
        int rayDirY = player.dy + player.dx * cameraX;

        // Which box of the map we're in
        int mapX = rayPosX;
        int mapY = rayPosY;

        // Length of ray from one x or y-side to next x or y-side
        int sideDistX;
        int sideDistY;

        // Length of ray from current position to next x or y-side
        int deltaDistX = (rayDirX == 0) ? 1 : abs(1 / rayDirX);
        int deltaDistY = (rayDirY == 0) ? 1 : abs(1 / rayDirY);

        int stepX;
        int stepY;

        int hit = 0; // Was there a wall hit?
        int side; // Was a NS or a EW wall hit?

        // Calculate step and initial sideDist
        if (rayDirX < 0) {
            stepX = -1;
            sideDistX = (rayPosX - mapX) * deltaDistX;
        } else {
            stepX = 1;
            sideDistX = (mapX + 1.0 - rayPosX) * deltaDistX;
        }
        if (rayDirY < 0) {
            stepY = -1;
            sideDistY = (rayPosY - mapY) * deltaDistY;
        } else {
            stepY = 1;
            sideDistY = (mapY + 1.0 - rayPosY) * deltaDistY;
        }

        // Perform DDA (Digital Differential Analysis)
        while (hit == 0) {
            // Jump to next map square, OR in x-direction, OR in y-direction
            if (sideDistX < sideDistY) {
                sideDistX += deltaDistX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaDistY;
                mapY += stepY;
                side = 1;
            }
            // Check if ray has hit a wall
            if (worldMap[mapX][mapY] > 0) hit = 1;
        }

        // Calculate distance projected on camera direction (Euclidean distance would give fisheye effect!)
        int perpWallDist = (side == 0) ? (mapX - rayPosX + (1 - stepX) / 2) / rayDirX : (mapY - rayPosY + (1 - stepY) / 2) / rayDirY;

        // Calculate height of line to draw on screen
        int lineHeight = perpWallDist ? (int)(SCREEN_HEIGHT / perpWallDist) : 60000;

        // Calculate lowest and highest pixel to fill in current stripe
        int drawStart = -lineHeight / 2 + SCREEN_HEIGHT / 2;
        if (drawStart < 0) drawStart = 0;
        int drawEnd = lineHeight / 2 + SCREEN_HEIGHT / 2;
        if (drawEnd >= SCREEN_HEIGHT) drawEnd = SCREEN_HEIGHT - 1;

        // Render line for this column
        for (int y = drawStart; y < drawEnd; y++) {
            putchar('#');
        }
        putchar('\n');
    }
}

int main() {
    Player player = {1, 1, 1, 0}; // Initial player position and direction
    render(player);
    return 0;
}


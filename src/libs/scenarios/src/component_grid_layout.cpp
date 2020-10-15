#include <queue>

#include <cassert>
#include <unordered_set>

#include <util/util.hpp>
#include <util/magnum.hpp>
#include <util/tiny_logger.hpp>

#include <scenarios/component_grid_layout.hpp>


using namespace VoxelWorld;


// Utils

namespace
{

Magnum::Vector3i directions[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};


struct CoordRange
{
    int min, max;
};


CoordRange startEndCoord(int bboxMin, int bboxMax, int direction)
{
    if (direction == 1)
        return {bboxMax + 1, bboxMax + 1};
    else if (direction == -1)
        return {bboxMin - 1, bboxMin - 1};
    else
        return {bboxMin, bboxMax};
}

std::vector<VoxelCoords> getFreeVoxels(const VoxelGrid<VoxelState> &grid, int length, int width, int startY)
{
    std::vector<VoxelCoords> res;

    for (int x = 1; x < length - 1; ++x)
        for (int z = 1; z < width - 1; ++z) {
            for (int y = startY; y > 0; --y) {
                const VoxelCoords coords{x, y - 1, z};
                const auto v = grid.get(coords);
                if (v && v->solid) {
                    res.emplace_back(x, y, z);
                    break;
                }
            }
        }

    return res;
}

}

// Layout generators

class GridLayoutComponent::GridLayoutImpl
{
public:
    explicit GridLayoutImpl(int numAgents, Rng &rng)
        : numAgents{numAgents}
          , rng{rng}
    {
    }

    virtual void init()
    {
        length = randRange(8, 30, rng);
        width = randRange(7, 25, rng);
    }

    virtual void generate(VoxelGrid<VoxelState> &grid) = 0;

    virtual std::vector<BoundingBox> extractPrimitives(VoxelGrid<VoxelState> &grid) = 0;

    virtual BoundingBox levelExit(const VoxelGrid<VoxelState> &grid) = 0;

    virtual BoundingBox buildingZone(const VoxelGrid<VoxelState> &grid) = 0;

    virtual std::vector<VoxelCoords> startingPositions(const VoxelGrid<VoxelState> &grid) = 0;

    virtual std::vector<VoxelCoords> objectSpawnPositions(const VoxelGrid<VoxelState> &grid) = 0;

public:
    int numAgents{};

    Rng &rng;

    // length = x, height = y, width = z
    int length{}, height{}, width{};
};


class LayoutGeneratorBasic : public GridLayoutComponent::GridLayoutImpl
{
public:
    explicit LayoutGeneratorBasic(int numAgents, Rng &rng)
        : GridLayoutImpl(numAgents, rng)
    {
    }

    void init() override
    {
        GridLayoutImpl::init();

        height = randRange(3, 5, rng);
    }

    void generate(VoxelGrid<VoxelState> &grid) override
    {
        // floor

        for (int x = 0; x < length; ++x)
            for (int z = 0; z < width; ++z)
                grid.set({x, 0, z}, VoxelState(true));

        // generating the perimeter walls

        for (int x = 0; x < length; x += length - 1)
            for (int y = 0; y < height; ++y)
                for (int z = 0; z < width; ++z)
                    grid.set({x, y, z}, VoxelState(true));

        for (int x = 0; x < length; ++x)
            for (int y = 0; y < height; ++y)
                for (int z = 0; z < width; z += width - 1)
                    grid.set({x, y, z}, VoxelState(true));
    }

    std::vector<BoundingBox> extractPrimitives(VoxelGrid<VoxelState> &grid) override
    {
        std::unordered_set<VoxelCoords> visited;

        const auto gridHashMap = grid.getHashMap();

        std::vector<BoundingBox> parallelepipeds;

        for (auto it : gridHashMap) {
            const auto &coord = it.first;
            const auto &voxel = it.second;
            if (!voxel.solid || visited.count(coord)) {
                // already processed this voxel
                continue;
            }

            visited.emplace(coord);

            BoundingBox bbox{coord, coord};
            std::vector<VoxelCoords> expansion;

            // try to expand the parallelepiped in every direction as far as we can
            for (auto direction : directions) {
                for (int sign = -1; sign <= 1; sign += 2) {
                    auto d = direction * sign;

                    bool canExpand = true;

                    // expanding in a specific direction as far as we can
                    while (true) {
                        const auto xlim = startEndCoord(bbox.min.x(), bbox.max.x(), d.x());
                        const auto ylim = startEndCoord(bbox.min.y(), bbox.max.y(), d.y());
                        const auto zlim = startEndCoord(bbox.min.z(), bbox.max.z(), d.z());

                        expansion.clear();

                        for (auto x = xlim.min; x <= xlim.max; ++x)
                            for (auto y = ylim.min; y <= ylim.max; ++y)
                                for (auto z = zlim.min; z <= zlim.max; ++z) {
                                    const VoxelCoords coords{x, y, z};
                                    const auto v = grid.get(coords);
                                    if (!v || !v->solid || visited.count(coords)) {
                                        // we could not expand in this direction
                                        canExpand = false;
                                        goto afterLoop;
                                    }

                                    expansion.emplace_back(coords);
                                }

                        afterLoop:

                        if (!canExpand)
                            break;

                        for (auto newVoxelCoord : expansion) {
                            visited.emplace(newVoxelCoord);
                            bbox.addPoint(newVoxelCoord);
                        }
                    }
                }
            }

            // finished expanding in all possible directions
            // the bounding box defines the parallepiped completely filled by solid voxels
            // we can draw only this parallelepiped (8 vertices) instead of drawing individual voxels, saving a ton of time
            parallelepipeds.emplace_back(bbox);
        }

        return parallelepipeds;
    }

    BoundingBox levelExit(const VoxelGrid<VoxelState> &) override
    {
        const auto exitPadWidth = std::min(3, numAgents);

        // make sure exit pad will fit
        assert(width - 2 >= exitPadWidth);

        const int xCoord = randRange(length - 2, length - 1, rng);
        const int zCoord = randRange(1, width - numAgents, rng);

        const VoxelCoords minCoord{xCoord, 1, zCoord}, maxCoord{xCoord + 1, 2, zCoord + exitPadWidth};

        return {minCoord, maxCoord};
    }

    BoundingBox buildingZone(const VoxelGrid<VoxelState> &) override
    {
        const VoxelCoords minCoord{0, 0, 0}, maxCoord{0, 0, 0};
        return {minCoord, maxCoord};
    }

    std::vector<VoxelCoords> startingPositions(const VoxelGrid<VoxelState> &) override
    {
        std::vector<VoxelCoords> agentPositions;

        for (int i = 0; i < numAgents; ++i) {
            for (int attempt = 0; attempt < 10; ++attempt) {
                const auto agentPos = VoxelCoords{randRange(1, length - 1, rng), 1, randRange(1, width - 1, rng)};

                if (!contains(agentPositions, agentPos)) {
                    agentPositions.emplace_back(agentPos);
                    break;
                }
            }
        }

        return agentPositions;
    }

    std::vector<VoxelCoords> objectSpawnPositions(const VoxelGrid<VoxelState> &) override
    {
        return std::vector<VoxelCoords>{};
    }
};


class LayoutGeneratorWalls : public LayoutGeneratorBasic
{
public:
    explicit LayoutGeneratorWalls(int numAgents, Rng &rng)
        : LayoutGeneratorBasic(numAgents, rng)
    {
    }

    void init() override
    {
        LayoutGeneratorBasic::init();

        const auto numWalls = randRange(0, maxNumWalls + 1, rng);
        const auto minLength = numWalls * 2 + 4 + 3;  // at least 2 voxels per wall + some space on either end

        length = randRange(minLength, 35, rng);

        if (numWalls <= 0) {

        } else {
            firstWallX = randRange(4, 4 + 1 + length - minLength, rng);
            auto firstWallHeight = randRange(1, tallestWall + 1, rng);
            maxWallX = firstWallX;
            maxWallHeight = firstWallHeight;

            walls.emplace_back(std::make_pair(firstWallX, firstWallHeight));

            auto prevWallX = firstWallX;

            for (int i = 1; i < numWalls; ++i) {
                const auto wallHeight = randRange(1, tallestWall + 1, rng);
                const auto remainingSpace = 3 + (numWalls - i - 1) * 2;

                if (prevWallX + 1 >= length - remainingSpace) {
                    TLOG(WARNING) << "Could not generate wall " << i << " not enough space!";
                    break;
                }

                const auto wallX = randRange(prevWallX + 1, length - remainingSpace, rng);
                prevWallX = wallX;

                walls.emplace_back(std::make_pair(wallX, wallHeight));
                maxWallHeight = std::max(maxWallHeight, wallHeight);
                maxWallX = std::max(maxWallX, wallX);
            }
        }

        height = randRange(3, 5, rng) + maxWallHeight;

        std::vector<VoxelCoords> spawnCandidates;
        for (int x = 1; x < firstWallX; ++x)
            for (int z = 1; z < width - 1; ++z)
                spawnCandidates.emplace_back(x, 1, z);

        std::shuffle(spawnCandidates.begin(), spawnCandidates.end(), rng);
        int spawnIdx = 0;
        agentSpawnCoords = std::vector<VoxelCoords>(spawnCandidates.begin(), spawnCandidates.begin() + numAgents);
        spawnIdx += numAgents;

        int minNumObjects = 0;
        for (const auto &wall : walls) {
            const auto wallHeight = wall.second;
            minNumObjects += (wallHeight - 1) * 2;
        }
        int numObjects = randRange(minNumObjects, minNumObjects + 4, rng);
        numObjects = std::min(numObjects, int(spawnCandidates.size() - spawnIdx));

        objectSpawnCoords = std::vector<VoxelCoords>(
            spawnCandidates.begin() + spawnIdx, spawnCandidates.begin() + spawnIdx + numObjects
        );
    }

    void generate(VoxelGrid<VoxelState> &grid) override
    {
        LayoutGeneratorBasic::generate(grid);

        for (const auto &wall : walls) {
            const auto wallX = wall.first;
            const auto wallHeight = wall.second;

            for (int y = 1; y < 1 + wallHeight; ++y)
                for (int z = 1; z < width - 1; ++z) {
                    VoxelCoords coord{wallX, y, z};
                    grid.set(coord, VoxelState(true));
                }
        }
    }

    std::vector<VoxelCoords> startingPositions(const VoxelGrid<VoxelState> &) override
    {
        return agentSpawnCoords;
    }

    std::vector<VoxelCoords> objectSpawnPositions(const VoxelGrid<VoxelState> &) override
    {
        return objectSpawnCoords;
    }

    BoundingBox levelExit(const VoxelGrid<VoxelState> &) override
    {
        const auto exitPadWidth = std::min(3, numAgents);
        // make sure exit pad will fit
        assert(width - 2 >= exitPadWidth);

        const auto exitX = randRange(maxWallX + 1, length - 1, rng);
        const auto exitZ = randRange(1, width - 1 - exitPadWidth, rng);

        VoxelCoords minCoord{exitX, 1, exitZ}, maxCoord{exitX + 1, 2, exitZ + exitPadWidth};
        return {minCoord, maxCoord};
    }

private:
    int maxWallHeight{}, maxWallX{}, firstWallX = 3;
    const int tallestWall = 4, maxNumWalls = 4;  // parameters (TODO curriculum)
    std::vector<std::pair<int, int>> walls;

    std::vector<VoxelCoords> agentSpawnCoords;
    std::vector<VoxelCoords> objectSpawnCoords;
};


class LayoutGeneratorCave : public LayoutGeneratorBasic
{
public:
    explicit LayoutGeneratorCave(int numAgents, Rng &rng)
        : LayoutGeneratorBasic(numAgents, rng)
    {
    }

    void init() override
    {
        LayoutGeneratorBasic::init();

        caveHeight = randRange(2, 5, rng);
        height = randRange(3, 5, rng) + caveHeight;
    }

    void generate(VoxelGrid<VoxelState> &grid) override
    {
        LayoutGeneratorBasic::generate(grid);

        // generate the actual cavity

        auto growthProb = 0.8f;

        std::queue<VoxelCoords> q;
        std::unordered_set<VoxelCoords> cave;

        auto numSeeds = std::max(1, std::max(length, width) / 7 + 1);

        for (auto seed = 0; seed < numSeeds; ++seed) {
            auto seedX = randRange(2, length - 2, rng);
            auto seedZ = randRange(2, width - 2, rng);

            auto initial = VoxelCoords {seedX, caveHeight, seedZ};
            cave.emplace(initial);
            q.emplace(initial);
        }

        while (!q.empty()) {
            auto curr = q.front();
            q.pop();

            for (auto direction : directions)
                for (int sign = -1; sign <= 1; sign += 2) {
                    auto d = sign * direction;
                    VoxelCoords newCoords{curr.x() + d.x(), curr.y() + d.y(), curr.z() + d.z()};

                    auto p = frand(rng);
                    if (p > growthProb)
                        continue;

                    if (cave.count(newCoords))
                        continue;

                    if (newCoords.y() > caveHeight || newCoords.y() < 1)
                        continue;

                    if (newCoords.x() >= length - 2 || newCoords.x() < 2)
                        continue;

                    if (newCoords.z() > width - 1 || newCoords.z() < 1)
                        continue;

                    q.emplace(newCoords);
                    cave.emplace(newCoords);
                    growthProb *= 0.995f;
                }
        }

        // generate the top surface

        for (int x = 1; x < length; ++x)
            for (int z = 1; z < width; ++z) {
                VoxelCoords coords{x, caveHeight, z};
                if (cave.count(coords))
                    continue;

                grid.set(coords, VoxelState(true));
            }

        // generate the walls of the cave

        for (const auto &coord : cave) {
            for (auto direction : directions)
                for (int sign = -1; sign <= 1; sign += 2) {
                    auto d = direction * sign;
                    VoxelCoords adjacent{coord.x() + d.x(), coord.y() + d.y(), coord.z() + d.z()};
                    if (adjacent.y() > caveHeight)
                        continue;

                    if (cave.count(adjacent))
                        continue;

                    grid.set(adjacent, VoxelState(true));
                }
        }

        // starting positions candidates
        freeVoxels = getFreeVoxels(grid, length, width, caveHeight + 1);
        std::shuffle(freeVoxels.begin(), freeVoxels.end(), rng);
    }

    std::vector<VoxelCoords> startingPositions(const VoxelGrid<VoxelState> &) override
    {
        return std::vector<VoxelCoords>(freeVoxels.begin(), freeVoxels.begin() + numAgents);
    }

    BoundingBox levelExit(const VoxelGrid<VoxelState> &grid) override
    {
        const auto exitPadWidth = std::min(3, numAgents);
        // make sure exit pad will fit
        assert(width - 2 >= exitPadWidth);

        VoxelCoords minCoord{1, 1, 1}, maxCoord{2, 2, 2};

        for (int i = int(freeVoxels.size()) - 1; i >= 0; --i) {
            const auto &v = freeVoxels[i];

            bool fits = true;
            for (int z = v.z(); z < v.z() + exitPadWidth; ++z) {
                const auto voxel = grid.get({v.x(), v.y(), z});
                if (voxel && voxel->solid) {
                    fits = false;
                    break;
                }
            }

            if (!fits) {
                // can't put exit pad here, try elsewhere
                continue;
            }

            minCoord = v;
            maxCoord = VoxelCoords{v.x() + 1, v.y() + 1, v.z() + exitPadWidth};
            break;
        }

        return {minCoord, maxCoord};
    }

private:
    int caveHeight{};

    std::vector<VoxelCoords> freeVoxels;
};


class LayoutGeneratorTower : public LayoutGeneratorBasic
{
public:
    explicit LayoutGeneratorTower(int numAgents, Rng &rng)
        : LayoutGeneratorBasic(numAgents, rng)
    {
    }

    void init() override
    {
        height = randRange(5, 7, rng);
        length = randRange(12, 30, rng);
        width = randRange(12, 25, rng);

        // determine the size and the position of the building zone
        buildZoneLength = randRange(3, 9, rng);
        buildZoneWidth = randRange(3, 9, rng);

        materialsLength = randRange(2, 8, rng);
        materialsWidth = randRange(2, 8, rng);

        length = std::max(buildZoneLength + materialsLength + 3, length);
        width = std::max(buildZoneWidth + materialsWidth + 3, width);

        buildZoneXOffset = randRange(1, length - buildZoneLength - 1, rng);
        buildZoneZOffset = randRange(1, width - buildZoneWidth - 1, rng);

        materialsXOffset = randRange(1, length - materialsLength - 1, rng);
        materialsZOffset = randRange(1, width - materialsWidth - 1, rng);

        std::vector<VoxelCoords> spawnCandidates;
        for (int x = 1; x < length - 1; ++x)
            for (int z = 1; z < width - 1; ++z)
                spawnCandidates.emplace_back(x, 2, z);

        std::shuffle(spawnCandidates.begin(), spawnCandidates.end(), rng);

        agentSpawnCoords = std::vector<VoxelCoords>(
            spawnCandidates.begin(), spawnCandidates.begin() + std::min(numAgents, int(spawnCandidates.size()))
        );
        auto spawnIdx = int(agentSpawnCoords.size());

        const auto maxRandomObjects = std::min(int(spawnCandidates.size()) - numAgents, 25);
        const auto spawnObjects = randRange(0, std::max(1, maxRandomObjects), rng);

        objectSpawnCoords = std::vector<VoxelCoords>(
            spawnCandidates.begin() + spawnIdx, spawnCandidates.begin() + spawnIdx + spawnObjects
        );

        for (auto &c : objectSpawnCoords) {
            if (c.x() >= materialsXOffset && c.x() < materialsXOffset + materialsLength
                && c.z() >= materialsZOffset && c.z() < materialsZOffset + materialsWidth) {
                continue;
            }

            c.y() -= 1;  // put the object on the floor
        }

        // add the main bulk of materials
        for (int x = materialsXOffset; x < materialsXOffset + materialsLength; ++x)
            for (int y = 1; y <= 1; ++y)
                for (int z = materialsZOffset; z < materialsZOffset + materialsWidth; ++z)
                    objectSpawnCoords.emplace_back(x, y, z);

        while (int(agentSpawnCoords.size()) < numAgents)
        agentSpawnCoords.emplace_back(agentSpawnCoords[0]);
    }

    void generate(VoxelGrid<VoxelState> &grid) override
    {
        LayoutGeneratorBasic::generate(grid);
    }

    std::vector<VoxelCoords> startingPositions(const VoxelGrid<VoxelState> &) override
    {
        return agentSpawnCoords;
    }

    std::vector<VoxelCoords> objectSpawnPositions(const VoxelGrid<VoxelState> &) override
    {
        return objectSpawnCoords;
    }

    BoundingBox levelExit(const VoxelGrid<VoxelState> &) override
    {
        VoxelCoords minCoord{0, 0, 0}, maxCoord{0, 0, 0};
        return {minCoord, maxCoord};
    }

    BoundingBox buildingZone(const VoxelGrid<VoxelState> &) override
    {
        const VoxelCoords minCoord{buildZoneXOffset, 1, buildZoneZOffset},
            maxCoord{buildZoneXOffset + buildZoneLength, 1, buildZoneZOffset + buildZoneWidth};
        return {minCoord, maxCoord};
    }

private:
    std::vector<VoxelCoords> agentSpawnCoords;
    std::vector<VoxelCoords> objectSpawnCoords;

    int buildZoneLength, buildZoneWidth, materialsLength, materialsWidth;
    int buildZoneXOffset, buildZoneZOffset, materialsXOffset, materialsZOffset;
};


// Wrapper class

GridLayoutComponent::GridLayoutComponent(Scenario &scenario, Rng &rng)
    : ScenarioComponent{scenario}
    , rng{rng}
{
}


GridLayoutComponent::~GridLayoutComponent() = default;


void GridLayoutComponent::init(int numAgents, LayoutType layoutType)
{
    switch (layoutType) {
        case LayoutType::Empty:
            generator = std::make_unique<LayoutGeneratorBasic>(numAgents, rng);
            break;
        case LayoutType::Walls:
            generator = std::make_unique<LayoutGeneratorWalls>(numAgents, rng);
            break;
        case LayoutType::Cave:
            generator = std::make_unique<LayoutGeneratorCave>(numAgents, rng);
            break;
        case LayoutType::Towers:
            generator = std::make_unique<LayoutGeneratorTower>(numAgents, rng);
            break;
        default:
            TLOG(ERROR) << "Layout type not supported " << int(layoutType);
            break;
    }

    generator->init();
}

void GridLayoutComponent::generate(VoxelGrid<VoxelState> &grid)
{
    generator->generate(grid);
}

std::vector<BoundingBox> GridLayoutComponent::extractPrimitives(VoxelGrid<VoxelState> &grid)
{
    return generator->extractPrimitives(grid);
}

BoundingBox GridLayoutComponent::levelExit(const VoxelGrid<VoxelState> &grid)
{
    return generator->levelExit(grid);
}

BoundingBox GridLayoutComponent::buildingZone(const VoxelGrid<VoxelState> &grid)
{
    return generator->buildingZone(grid);
}

std::vector<VoxelCoords> GridLayoutComponent::startingPositions(const VoxelGrid<VoxelState> &grid)
{
    return generator->startingPositions(grid);
}

std::vector<VoxelCoords> GridLayoutComponent::objectSpawnPositions(const VoxelGrid<VoxelState> &grid)
{
    return generator->objectSpawnPositions(grid);
}

void VoxelWorld::GridLayoutComponent::addLayoutDrawables(DrawablesMap &drawables, Env::EnvState &envState,
                                                         VoxelGrid<VoxelState> &grid)
{
    collisionShapes.clear();

    {
        auto layoutDrawables = generator->extractPrimitives(grid);

        TLOG(INFO) << "Env has " << layoutDrawables.size() << " layout drawables";

        for (auto layoutDrawable : layoutDrawables) {
            const auto bboxMin = layoutDrawable.min, bboxMax = layoutDrawable.max;
            auto scale = Magnum::Vector3{
                float(bboxMax.x() - bboxMin.x() + 1) / 2,
                float(bboxMax.y() - bboxMin.y() + 1) / 2,
                float(bboxMax.z() - bboxMin.z() + 1) / 2,
            };

            auto bBoxShape = std::make_unique<btBoxShape>(btVector3{scale.x(), scale.y(), scale.z()});
            // auto bBoxShape = std::make_unique<btBoxShape>(btVector3{1, 1, 1});
            auto &layoutObject = envState.scene->addChild<RigidBody>(envState.scene.get(), 0.0f, bBoxShape.get(), envState.physics.bWorld);

            auto translation = Magnum::Vector3{
                float((bboxMin.x() + bboxMax.x())) / 2 + 0.5f,
                float((bboxMin.y() + bboxMax.y())) / 2 + 0.5f,
                float((bboxMin.z() + bboxMax.z())) / 2 + 0.5f
            };

            layoutObject.scale(scale).translate(translation);
            layoutObject.syncPose();

            drawables[DrawableType::Box].emplace_back(&layoutObject, rgb(ColorRgb::LAYOUT));

            collisionShapes.emplace_back(std::move(bBoxShape));
        }
    }

    {
        const auto objectSpawnPositions = generator->objectSpawnPositions(grid);

        const auto objSize = 0.39f;
        auto objScale = Magnum::Vector3{objSize, objSize, objSize};

        for (const auto &movableObject : objectSpawnPositions) {
            const auto pos = movableObject;
            auto translation = Magnum::Vector3{float(pos.x()) + 0.5f, float(pos.y()) + 0.5f, float(pos.z()) + 0.5f};

            auto bBoxShape = std::make_unique<btBoxShape>(btVector3{0.45f, 0.5f, 0.45f});

            auto &object = envState.scene->addChild<RigidBody>(envState.scene.get(), 0.0f, bBoxShape.get(), envState.physics.bWorld);
            object.scale(objScale).translate(translation);
            object.setCollisionOffset({0.0f, -0.1f, 0.0f});
            object.syncPose();

            drawables[DrawableType::Box].emplace_back(&object, rgb(ColorRgb::MOVABLE_BOX));

            collisionShapes.emplace_back(std::move(bBoxShape));

            VoxelState voxelState{false};
            voxelState.obj = &object;
            grid.set(pos, voxelState);
        }
    }

    // exit pad
    {
        const auto exitPadCoords = generator->levelExit(grid);
        const auto exitPadScale = Magnum::Vector3(
            exitPadCoords.max.x() - exitPadCoords.min.x(),
            1.0,
            exitPadCoords.max.z() - exitPadCoords.min.z()
        );

        if (exitPadScale.x() > 0) {
            // otherwise we don't need the exit pad
            const auto exitPadPos = Magnum::Vector3(exitPadCoords.min.x() + exitPadScale.x() / 2, exitPadCoords.min.y(), exitPadCoords.min.z() + exitPadScale.z() / 2);

            auto &exitPadObject = envState.scene->addChild<Object3D>(envState.scene.get());
            exitPadObject.scale({0.5, 0.025, 0.5}).scale(exitPadScale);
            exitPadObject.translate({0.0, 0.025, 0.0});
            exitPadObject.translate(exitPadPos);

            drawables[DrawableType::Box].emplace_back(&exitPadObject, rgb(ColorRgb::EXIT_PAD));
        }
    }

    // building zone
    {
        const auto buildingZone = generator->buildingZone(grid);

        const auto zoneScale = Magnum::Vector3(buildingZone.max.x() - buildingZone.min.x(), 1.0, buildingZone.max.z() - buildingZone.min.z());

        if (zoneScale.x() > 0) {
            // otherwise we don't need the zone
            const auto zonePos = Magnum::Vector3(buildingZone.min.x() + zoneScale.x() / 2, buildingZone.min.y(),
                                                 buildingZone.min.z() + zoneScale.z() / 2);

            auto &zoneObject = envState.scene->addChild<Object3D>(envState.scene.get());
            zoneObject.scale({0.55, 0.075, 0.55}).scale(zoneScale);
            zoneObject.translate({0.0, 0.055, 0.0});
            zoneObject.translate(zonePos);

            drawables[DrawableType::Box].emplace_back(&zoneObject, rgb(ColorRgb::BUILDING_ZONE));
        }
    }
}
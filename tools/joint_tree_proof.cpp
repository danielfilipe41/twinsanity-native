// Headless proof of an OGI's joint tree (no disc needed): the animations SetJointAnimations builds from a model's joints.
//
// retail walks a model's joints in index order and its own data has every parent before its children, so a joint's parent
// animation is always the one the step before made. A model whose joints are not in that order had a null parent there, and
// GetJointAnimationFromParentJoint read the child count through it -- the crash at null + 0x54 (JointAnimation::bits). The
// tree is built from the parent now (animation.cpp's JointAnimationOfJoint), and these checks pin what that has to keep:
// whatever order the joints are listed in, every joint the data describes ends up in the tree under its own parent, once,
// and the cases retail's data cannot produce (a joint that is its own parent, a parent past the joints, more children than
// an animation holds) leave no crash and no bogus joint behind.
//
// Build: ninja joint-tree-proof && ./build/host/joint_tree_proof   (from the repo root)
#include "game/animation.h"
#include "game/memory.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
bool g_Ok = true;

bool Check(bool condition, const char* what)
{
    if (!condition)
    {
        g_Ok = false;
        std::printf("FAIL  %s\n", what);
    }

    return condition;
}

bool CheckEqual(s32 got, s32 want, const char* what)
{
    if (got != want)
    {
        g_Ok = false;
        std::printf("FAIL  %s (got %d, want %d)\n", what, got, want);
        return false;
    }

    return true;
}

void Phase(const char* name)
{
    std::printf("--- %s\n", name);
}

// A model's skeleton: the OGI, the joints it points at and their exit points, the joints' parents given as indices (a
// negative one is GameOGI::NoJoint, a joint with no parent)
struct Skeleton
{
    GameOGI ogi;
    std::vector<OgiJoint> joints;
    std::vector<OgiExitPoint> exitPoints;
};

void MakeSkeleton(Skeleton* skeleton, const s32* parents, u32 jointCount)
{
    std::memset(&skeleton->ogi, 0, sizeof(skeleton->ogi));
    skeleton->joints.assign(jointCount, OgiJoint());
    for (u32 index = 0; index < jointCount; index++)
    {
        OgiJoint& joint = skeleton->joints[index];
        joint.index = static_cast<u8>(index);
        joint.id = static_cast<u8>(index);
        joint.parent = parents[index] < 0 ? GameOGI::NoJoint : static_cast<u8>(parents[index]);
    }

    skeleton->ogi.jointCount = static_cast<u8>(jointCount);
    skeleton->ogi.joints = jointCount != 0 ? skeleton->joints.data() : nullptr;
    skeleton->exitPoints.clear();
    skeleton->ogi.exitPointCount = 0;
    skeleton->ogi.exitPoints = nullptr;
}

// An exit point on a joint (the index as the model's data has it, which can name no joint or one past its joints)
void AddExitPoint(Skeleton* skeleton, u8 joint)
{
    OgiExitPoint point = OgiExitPoint();
    point.joint = joint;
    point.id = static_cast<u8>(skeleton->exitPoints.size());
    skeleton->exitPoints.push_back(point);
    skeleton->ogi.exitPointCount = static_cast<u8>(skeleton->exitPoints.size());
    skeleton->ogi.exitPoints = skeleton->exitPoints.data();
}

// The animator an instance's model makes: the same call ModelNode::SetOgi makes the first time it is given an OGI
OgiAnimator* BuildTree(Skeleton* skeleton)
{
    // No react joints, and an exit point's animation for each the model has (the counts ModelNode::SetOgi hands over from
    // its object's header)
    return InitOgiAnimatorService(static_cast<OgiAnimator*>(MemoryAllocate(sizeof(OgiAnimator))), &skeleton->ogi, 0,
                                  static_cast<u32>(skeleton->exitPoints.size()));
}

// The animations under a joint, walked through the children each one lists, counting what is left so a tree that loops
// back on itself is reported rather than walked for good
u32 CountTree(JointAnimation* joint, u32 budget)
{
    if (joint == nullptr)
    {
        return 0;
    }

    if (budget == 0)
    {
        return 0;
    }

    u32 count = 1;
    const u32 children = joint->bits.childCount;
    if (!(children <= JointAnimation::MostChildren))
    {
        return count;
    }

    for (u32 child = 0; child < children && count < budget; child++)
    {
        count += CountTree(joint->children[child], budget - count);
    }

    return count;
}

// How many of the animations are the same one (a joint made twice would be in the tree twice)
u32 CountRepeats(JointAnimation** animations, u32 count)
{
    u32 repeats = 0;
    for (u32 first = 0; first < count; first++)
    {
        for (u32 second = first + 1; second < count; second++)
        {
            if (animations[first] != nullptr && animations[first] == animations[second])
            {
                repeats++;
            }
        }
    }

    return repeats;
}

// Every joint the data has an animation: its own joint, under its own parent's animation (or the root, for a joint that
// names no parent), and the tree from the root holds each of them exactly once
void CheckTree(Skeleton* skeleton, const char* name)
{
    const u32 jointCount = skeleton->joints.size();
    OgiAnimator* animator = BuildTree(skeleton);
    Phase(name);

    if (!Check(animator->joints != nullptr, "the animator has its joints' table"))
    {
        DeleteOgiAnimator(animator);
        return;
    }

    CheckEqual(static_cast<s32>(animator->joints->size), static_cast<s32>(jointCount), "the table is the model's joint count");

    u32 made = 0;
    u32 roots = 0;
    for (u32 index = 0; index < jointCount; index++)
    {
        JointAnimation* animation = animator->joints->data[index];
        if (animation == nullptr)
        {
            continue;
        }

        made++;
        Check(animation->joint == &skeleton->joints[index], "an animation is its own joint's");
        const u8 parent = skeleton->joints[index].parent;
        if (parent == GameOGI::NoJoint)
        {
            roots++;
            Check(animation == animator->root, "a joint with no parent is under the animator's root");
            Check(animation->parent == nullptr, "the root has no parent's animation");
        }
        else if (parent == index)
        {
            roots++;
            Check(animation == animator->root, "a joint that names itself is the root");
        }
        else
        {
            // A parent the walk passed (that is the crash this is here for) has no animation of its own, and the joint under
            // it is the second failure reported
            const JointAnimation* parentAnimation = animator->joints->data[parent];
            if (!Check(parentAnimation != nullptr, "a joint's parent is made too"))
            {
                continue;
            }

            Check(animation->parent == parentAnimation, "a joint is under its parent's animation");
        }
    }

    CheckEqual(static_cast<s32>(made), static_cast<s32>(jointCount), "every joint has an animation");
    // The walk from the root: the same joints, and no joint in it twice
    CheckEqual(static_cast<s32>(animator->root != nullptr ? 1u : 0u), static_cast<s32>(roots != 0 ? 1u : 0u),
               "the root is made exactly when a joint names no parent");
    CheckEqual(static_cast<s32>(CountTree(animator->root, jointCount + 1)), static_cast<s32>(made),
               "the root's tree holds every joint it made");
    CheckEqual(static_cast<s32>(CountRepeats(animator->joints->data, jointCount)), 0, "no joint is in the tree twice");

    DeleteOgiAnimator(animator);
}
}

int main()
{
    // A chain listed child first: joint 0's parent is 1, joint 1's parent is 2, joint 2 is the root. This is the shape that
    // crashed: the walk reached joint 0 while its parent's animation was still null
    {
        const s32 parents[] = {1, 2, -1};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 3);
        CheckTree(&skeleton, "a chain listed child first");

        OgiAnimator* animator = BuildTree(&skeleton);
        Check(animator->joints->data[0] != nullptr && animator->joints->data[0]->parent == animator->joints->data[1],
              "the child's animation is under its parent's, not under nothing");
        Check(animator->joints->data[1]->parent == animator->root, "the middle joint is under the root");
        DeleteOgiAnimator(animator);
    }

    // The order retail's own data is in: parents before children, a root and its children and a grandchild
    {
        const s32 parents[] = {-1, 0, 0, 2};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 4);
        CheckTree(&skeleton, "a skeleton listed parent first");
    }

    // A joint that names itself: a model's data cannot have one, and the tree must not follow it round
    {
        const s32 parents[] = {0};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 1);
        CheckTree(&skeleton, "a joint that is its own parent");
    }

    // A parent past the model's joints: no animation for it, and the other joints are still made
    {
        const s32 parents[] = {9, -1};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 2);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("a parent past the joints");
        Check(animator->joints->data[0] == nullptr, "a joint whose parent is past the table has no animation");
        Check(animator->joints->data[1] == animator->root, "the root beside it is made");
        DeleteOgiAnimator(animator);
    }

    // More children on one joint than an animation holds (JointAnimation::MostChildren): the ones past the room have no
    // animation, the rest are made, and nothing is read past the children array
    {
        constexpr u32 ChildCount = JointAnimation::MostChildren + 2;
        s32 parents[ChildCount];
        parents[0] = -1;
        for (u32 child = 1; child < ChildCount; child++)
        {
            parents[child] = 0;
        }

        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, ChildCount);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("more children than an animation holds");
        CheckEqual(static_cast<s32>(animator->root->bits.childCount), static_cast<s32>(JointAnimation::MostChildren),
                   "the root holds the room it has and no more");
        Check(animator->joints->data[JointAnimation::MostChildren + 1] == nullptr,
              "a child past the room has no animation");
        CheckEqual(static_cast<s32>(CountTree(animator->root, ChildCount + 1)),
                   static_cast<s32>(JointAnimation::MostChildren + 1), "the tree is the root and the children it holds");
        DeleteOgiAnimator(animator);
    }

    // An exit point on a joint the model has: its animation is that joint's, and the point's matrix is on it
    {
        const s32 parents[] = {-1, 0};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 2);
        AddExitPoint(&skeleton, 1);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("an exit point on a joint");
        Check(animator->exitPoints != nullptr && animator->exitPoints->data[0]->joint == animator->joints->data[1],
              "the exit point is bound to its joint's animation");
        DeleteOgiAnimator(animator);
    }

    // An exit point naming a joint past the model's, and one on a model with no joints at all: no joint to be on, and the
    // read past the joints' table must not happen
    {
        const s32 parents[] = {-1, 0};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 2);
        AddExitPoint(&skeleton, 9);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("an exit point past the joints");
        Check(animator->exitPoints != nullptr && animator->exitPoints->data[0]->joint == nullptr,
              "a point past the joints has no animation");
        DeleteOgiAnimator(animator);

        Skeleton empty;
        MakeSkeleton(&empty, nullptr, 0);
        AddExitPoint(&empty, 0);
        OgiAnimator* emptyAnimator = BuildTree(&empty);
        Phase("an exit point on a model with no joints");
        Check(emptyAnimator->exitPoints != nullptr && emptyAnimator->exitPoints->data[0]->joint == nullptr,
              "a point on no joints has no animation");
        DeleteOgiAnimator(emptyAnimator);
    }

    // A joint matrix asked for by an index the animator does not have (a hull's joint from another model's data): nothing
    // copied, rather than the words past the joints' table
    {
        const s32 parents[] = {-1, 0};
        Skeleton skeleton;
        MakeSkeleton(&skeleton, parents, 2);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("a joint matrix asked for by an index past the joints");
        Matrix4x4 matrix;
        std::memset(&matrix, 0, sizeof(matrix));
        Check(CopyJointTransform(animator, 5, &matrix) == 0, "an index past the joints copies nothing");
        Check(CopyJointTransform(animator, 0, &matrix) == 1, "a joint the animator has is copied");
        DeleteOgiAnimator(animator);
    }

    // No joints at all: nothing made, nothing to walk
    {
        Skeleton skeleton;
        MakeSkeleton(&skeleton, nullptr, 0);
        OgiAnimator* animator = BuildTree(&skeleton);
        Phase("a model with no joints");
        Check(animator->root == nullptr, "there is no root");
        Check(animator->joints == nullptr || animator->joints->data == nullptr, "there is no joints' table to walk");
        Check(animator->joints == nullptr || animator->joints->size == 0, "the empty table is empty");
        DeleteOgiAnimator(animator);
    }

    std::printf("joint-tree-proof: %s\n", g_Ok ? "PASS" : "FAIL");
    return g_Ok ? 0 : 1;
}

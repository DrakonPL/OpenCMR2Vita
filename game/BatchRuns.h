// PORT: the order of a run of stage objects for the tree batch
// (Game_DrawStaticStageObjects). The batch draws when the next object has
// another state than the one before (the cull mode, the lighting). The
// objects of a stage alternate between two or three states, so the batch
// drew for nearly each object: some 3,000 instructions each time. The
// objects of a run are solid and write the depth: the picture does not
// depend on their order. This order puts the objects of one state together.
// No game state here: tests/batch_runs.cpp checks it on the PC.
#ifndef OPENCMR2_BATCH_RUNS_H
#define OPENCMR2_BATCH_RUNS_H

// The most states in a run that are put together. A run with more keeps its
// order.
enum { kBatchRunStates = 16 };

// order[0..count) gets the indices of state[0..count): the objects of one
// state together and in their order, the states in the order of their last
// object. So the last object of the run is the last of the order, and the
// state that the run leaves is the same as before. count is at most 65,535.
inline void BatchRun_Order(const unsigned *state, unsigned count, unsigned short *order)
{
    unsigned states[kBatchRunStates];
    unsigned last[kBatchRunStates];
    unsigned stateCount = 0;
    unsigned i, s;

    for (i = 0; i < count; i++) {
        for (s = 0; s < stateCount && states[s] != state[i]; s++)
            ;
        if (s == stateCount) {
            if (stateCount == kBatchRunStates) {
                for (i = 0; i < count; i++)
                    order[i] = (unsigned short)i;
                return;
            }
            states[stateCount++] = state[i];
        }
        last[s] = i;
    }
    // The states by their last object: a few, by insertion.
    for (s = 1; s < stateCount; s++) {
        const unsigned movedState = states[s], movedLast = last[s];
        unsigned at = s;
        for (; at > 0 && last[at - 1] > movedLast; at--) {
            states[at] = states[at - 1];
            last[at] = last[at - 1];
        }
        states[at] = movedState;
        last[at] = movedLast;
    }
    unsigned out = 0;
    for (s = 0; s < stateCount; s++)
        for (i = 0; i <= last[s]; i++)
            if (state[i] == states[s])
                order[out++] = (unsigned short)i;
}

#endif

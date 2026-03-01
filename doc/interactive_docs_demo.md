# Interactive Docs Demo Implementation

This document describes the current behavior of `data/interactive-docs.html`.

## Orientation

The diagram is now rendered as real `stateDiagram-v2` again (not converted to flowchart).  
That restores the rounded-state look and vertical layout style you preferred.

## Mermaid Runtime

Yes, it still uses original Mermaid:

- Script loaded: `data/mermaid.min.js`
- Version: Mermaid `10.9.1` official minified build
- No custom Mermaid engine code is used.

## Input Handling

The textarea accepts state transition lines like:

```text
stateDiagram-v2
Idle --> Precharge: Start
Precharge --> Ready: Voltage ok
...
```

`buildDiagramSource()` now returns this state diagram source directly (adds `stateDiagram-v2` only if missing).

## Live Simulation Logic

State stepping is driven by:

- `transitions[]` parsed from the input
- `currentState`
- `lastTransitionIndex`
- timer via `setInterval(stepSimulation, delayMs)`

Each step:

1. Updates metric values (temperature/voltage drift)
2. Advances to next transition
3. Re-renders Mermaid
4. Applies visual highlighting

## Color Coding (Current Method)

Coloring is **post-render SVG decoration**, not Mermaid inline style directives.

After Mermaid renders:

- Current state node gets:
  - `stroke: green`
  - `stroke-width: 3`
  - `fill: #dcfce7`
- Last transition path gets:
  - `stroke: green`
  - `stroke-width: 4`

This avoids Mermaid parser issues from inline `classDef/linkStyle` in this scenario.

## Live Values In States

Live values are drawn as extra SVG text labels near each state node:

- Format: `T <temp> C | V <voltage> V`
- Updated every simulation step
- Source data from `stateMetrics[stateName]`

Additionally, the status line below controls shows current state's live values.

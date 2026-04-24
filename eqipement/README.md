# JSON-Driven Interaction Specification

## Overview

This document describes a simple JSON-based event format used to drive
system interactions. Each JSON message represents a single event that
can either trigger a system action or report an error.

------------------------------------------------------------------------

## Message Structure

Each message follows this structure:

``` json
{
  "timestamp": 123456,
  "event": {
    "type": "system",
    "action": "speed",
    "value": -10
  }
}
```

------------------------------------------------------------------------

## Fields Description

### `timestamp`

-   **Type:** Number (integer)
-   **Description:**\
    Represents the time when the event was generated.\
    Typically expressed as a Unix timestamp or another agreed-upon time
    format.

------------------------------------------------------------------------

### `event`

-   **Type:** Object\
-   **Description:**\
    Contains the core event details.

#### `event.type`

-   **Type:** String\
-   **Allowed values:**
    -   `system`
    -   `error`
-   **Description:**\
    Defines the nature of the event:
    -   `system` → triggers an action in the system
    -   `error` → indicates a failure or issue

------------------------------------------------------------------------

#### `event.action`

-   **Type:** String\
-   **Required when:** `event.type = "system"`\
-   **Description:**\
    Specifies the action that the system should execute.

------------------------------------------------------------------------

#### `event.value`

-   **Type:** Number (or depends on action)\
-   **Required when:** `event.type = "system"`\
-   **Description:**\
    Provides the value associated with the action.

------------------------------------------------------------------------

## Event Types Behavior

### 1. System Event

When `event.type = "system"`: - The system **must execute** the
specified `action` - The `value` is passed as a parameter to that action

------------------------------------------------------------------------

### 2. Error Event

When `event.type = "error"`: - No system action is executed - The event
represents a failure or issue

------------------------------------------------------------------------

## Validation Rules

-   `timestamp` must be present and valid
-   `event.type` must be either `system` or `error`
-   If `event.type = "system"`:
    -   `action` is required
    -   `value` is required
-   If `event.type = "error"`:
    -   `action` and `value` are optional

------------------------------------------------------------------------

## Extensibility

The format can be extended with additional fields if needed.

------------------------------------------------------------------------

## Summary

-   JSON messages represent discrete events
-   `system` events trigger actions
-   `error` events report issues
-   The structure is simple and extensible

# Events

Every call on this module returns as soon as the request is dispatched. What
actually happened on the network arrives later, as an event.

Events are declared in the `logos_events:` section of
`src/delivery_module_plugin.h` and emitted off-thread. Subscribe to them rather
than reading a return value — a successful `send` means "accepted for
dispatch", nothing more.

## Correlating a call with its events

`send` and `channelSend` return a **request id** on success. Every event that
reports the outcome of that call carries the same id, so a caller that has
several messages in flight can tell them apart.

`start` and `stop` carry no id; they report through `nodeStarted` and
`nodeStopped`.

## Timestamps

All timestamps are **`int64` nanoseconds since the Unix epoch**.

One exception is worth knowing: `messageReceived` reports the timestamp
carried by the message itself, so it reflects when the message was created
rather than when this node saw it. Every other event is stamped by the module
host from its own realtime clock at the moment the event is emitted.

## Message events

| Event | Parameters | Meaning |
| ----- | ---------- | ------- |
| `messageSent` | `requestId`, `messageHash`, `timestamp` | The network validated the message. This is the success terminal state for `send`. |
| `messagePropagated` | `requestId`, `messageHash`, `timestamp` | The message reached the network but is not yet validated. |
| `messageError` | `requestId`, `messageHash`, `error`, `timestamp` | The module could not send the message. `error` carries the reason. |
| `messageReceived` | `messageHash`, `contentTopic`, `payload`, `timestamp` | A message arrived on a subscribed topic. |

A single `send` typically produces `messagePropagated` followed by
`messageSent`, or `messageError` on failure.

## Reliable channel events

| Event | Parameters | Meaning |
| ----- | ---------- | ------- |
| `channelMessageSent` | `channelId`, `requestId`, `timestamp` | Every segment of the send was confirmed. |
| `channelMessageError` | `channelId`, `requestId`, `error`, `timestamp` | The send finalised with a failed segment. |
| `channelMessageReceived` | `channelId`, `senderId`, `payload`, `timestamp` | A message arrived on an open channel. `senderId` is the sending participant's SDS identifier. |

## Node lifecycle events

| Event | Parameters | Meaning |
| ----- | ---------- | ------- |
| `nodeStarted` | `success`, `message`, `timestamp` | `start` finished. `message` carries the failure reason when `success` is false. |
| `nodeStopped` | `success`, `message`, `timestamp` | `stop` finished. |

## Connectivity

| Event | Parameters | Meaning |
| ----- | ---------- | ------- |
| `connectionStateChanged` | `connectionStatus`, `timestamp` | The node's connectivity changed. |

## Payloads

`messageReceived` and `channelMessageReceived` deliver `payload` as raw bytes.
The two travel over the FFI boundary in different encodings — a JSON array of
byte values for the former, base64 for the latter — but both are decoded
before they reach you, so no unwrapping is needed on either.

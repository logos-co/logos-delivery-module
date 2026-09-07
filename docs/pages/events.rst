Events
======

.. note::

    A caller never invokes these.

    Every method returns as soon as its request is dispatched, and what
    actually happened on the network arrives here — so subscribe to these
    rather than reading a return value.

``send`` and ``channelSend`` return a request id, and every event reporting the
outcome of that call carries the same id, so several messages can be in flight
at once.

Timestamps are ``int64`` nanoseconds since the Unix epoch. ``messageReceived``
reports the timestamp carried by the message itself; every other event is
stamped by the module host when the event is emitted.

.. doxygengroup:: events
   :content-only:

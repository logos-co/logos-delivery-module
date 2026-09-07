API layers
==========

logos-delivery is built in three layers. Each one adds guarantees on top of the
one below, and you pick how far up the stack you want to work.

.. code-block:: text

   Reliable Channels     ordered, tracked delivery between participants
        ▲
   Messaging API         publish and subscribe on a content topic
        ▲
   Kernel                the p2p protocols themselves

Kernel
------

The protocols a node actually speaks — relay, filter, lightpush, store,
discovery, peer management. This is the transport, and everything above is
built on it.

.. warning::

   The Kernel API is an unstable, unsupported surface. It exposes per-protocol
   internals and may change or be removed at any time, without notice and
   without a deprecation cycle. Use it at your own risk.

Messaging API
-------------

Publish and subscribe on a content topic. You hand it a topic and a payload;
it reaches whoever is subscribed. Delivery is best-effort: a send is reported
as it progresses, but nothing is retried or acknowledged for you.

This is the stable surface, and the right layer for most applications.

Reliable Channels
-----------------

A channel is a durable conversation between participants on a content topic.
Sends are tracked to completion rather than fired and forgotten, and each
participant is identified, so the layer can tell you when a message is fully
delivered or has failed. Channel state persists, so a closed channel can be
reopened where it left off.

Also a stable surface. Use it when "did that actually arrive?" matters.

What this module exposes
------------------------

The **Messaging API** and **Reliable Channels** in full — creating and running
a node, ``send`` / ``subscribe`` / ``unsubscribe``, and the ``channel*``
methods, along with the events each of them reports.

From the **Kernel**, one method only: :doc:`storeQuery <api_reference>`, for
reading historical messages back off a store node. It carries the Kernel
warning above — its JSON contract follows the kernel API and can change with
it.

How much of the stack a node mounts is a configuration choice, made with
``entryLayer`` when you create the node. A kernel-only node can still answer
``storeQuery``, report node info and serve metrics, but ``send``, ``subscribe``
and the ``channel*`` methods will fail on it. See
:doc:`Architecture <architecture>` for what each setting mounts, and the
:doc:`API reference <api_reference>` for the configuration grammar.

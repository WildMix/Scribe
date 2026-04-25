# `shardCollection` Event

ce-shardCollection## Summary

`shardCollection`
A `shardCollection` event occurs when a collection is sharded.

## Description

<table>
<tr>
<th id="Field">
Field

</th>
<th id="Type">
Type

</th>
<th id="Description">
Description

</th>
</tr>
<tr>
<td headers="Field">
`_id`

</td>
<td headers="Type">
Document

</td>
<td headers="Description">
A [BSON](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-BSON) object, which serves as an identifier for the change stream event. This value is used as the `resumeToken` for the `resumeAfter` parameter when resuming a change stream. The fields within the `_id` object depend on the MongoDB versions and, in some cases, the [feature compatibility version (FCV)](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/setFeatureCompatibilityVersion/#std-label-view-fcv) at the time of the change stream's opening or resumption.

For an example of resuming a change stream by `resumeToken`, see [Resume a Change Stream](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume).

</td>
</tr>
<tr>
<td headers="Field">
`clusterTime`

</td>
<td headers="Type">
Timestamp

</td>
<td headers="Description">
`clusterTime` is the timestamp from the oplog entry associated with the event.

Due to [oplog size limits](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/transactions-production-consideration/#std-label-txn-oplog-size-limit), [multi-document transactions](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/transactions/#std-label-transactions) may create multiple oplog entries. In a transaction, change stream events staged in a given oplog entry share the same `clusterTime`.

Events with the same `clusterTime` may not all relate to the same transaction. Some events don't relate to a transaction at all. Starting in MongoDB 8.0, this may be true for events on any deployment. In previous versions, this behavior was possible only for events on a sharded cluster.

To identify events for a single transaction, you can use the combination of `lsid` and `txnNumber` in the change stream event document.

</td>
</tr>
<tr>
<td headers="Field">
`collectionUUID`

</td>
<td headers="Type">
UUID

</td>
<td headers="Description">
UUID (Universally Unique Identifier) identifying the collection where the change occurred.

</td>
</tr>
<tr>
<td headers="Field">
`lsid`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
The identifier for the session associated with the transaction.

Only present if the operation is part of a [multi-document transaction](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/transactions/).

</td>
</tr>
<tr>
<td headers="Field">
`ns`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
The namespace (database and or collection) affected by the event.

</td>
</tr>
<tr>
<td headers="Field">
`ns.coll`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The name of the collection where the event occurred.

</td>
</tr>
<tr>
<td headers="Field">
`ns.db`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The name of the database where the event occurred.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
Additional information on the change operation.

This document and its subfields only appears when the change stream uses [expanded events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events).

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``presplitHashedZones`

</td>
<td headers="Type">
boolean

</td>
<td headers="Description">
Indicates whether the shard chunks were [pre-split](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/sh.shardCollection/#std-label-method-shard-collection-presplitHashedZones) according to zones when the collection becamed sharded.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``shardKey`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
The [shard key](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/sharding-shard-key/#std-label-shard-key) for the collection where the change occurred.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``unique`

</td>
<td headers="Type">
boolean

</td>
<td headers="Description">
This has a value of true if the collection was sharded with a unique shard key.

</td>
</tr>
<tr>
<td headers="Field">
`operationType`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The type of operation that the change notification reports.

Returns a value of `shardCollection` for these change events.

</td>
</tr>
<tr>
<td headers="Field">
`txnNumber`

</td>
<td headers="Type">
NumberLong

</td>
<td headers="Description">
Together with the [lsid](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/shardCollection/#std-label--idref--lsid), a number that helps uniquely identify a transction.

Only present if the operation is part of a [multi-document transaction](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/transactions/).

</td>
</tr>
<tr>
<td headers="Field">
`wallTime`

</td>
<td headers="Type">
[ISODate](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-ISODate)

</td>
<td headers="Description">
The server date and time of the database operation. `wallTime` differs from `clusterTime` in that `clusterTime` is a timestamp taken from the oplog entry associated with the database operation event.

</td>
</tr>
</table>

## Example

The following example shows a `shardCollection` event:

```json
{
   "_id": { <ResumeToken> },
   "operationType": "shardCollection",
   "clusterTime": Timestamp({ t: 1654894852, i: 52 }),
   "collectionUUID": UUID("98046a1a-b649-4e5b-9c75-67594221ce19"),
   "wallTime": ISODate("2022-06-10T21:00:52.854Z"),
   "ns": {
      "db": "test",
      "coll": "authors"
   },
   "operationDescription": {
      "shardKey": { "age": "hashed" },
      "unique": false,
      "presplitHashedZones": false
   }
}
```


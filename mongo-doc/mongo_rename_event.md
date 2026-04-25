# refineCollectionShardKey Event

ce-refineCollectionShardKey## Summary

`refineCollectionShardKey`
A `refineCollectionShardKey` event occurs when a collection's shard key is modified.

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
`ns`

</td>
<td headers="Type">
Document

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
String

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
String

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
Document

</td>
<td headers="Description">
Additional information on the change operation.

This document and its subfields only appears when the change stream uses [expanded events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events).

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``shardKey`

</td>
<td headers="Type">
Document

</td>
<td headers="Description">
The [shard key](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/sharding-shard-key/#std-label-shard-key) for the collection where the change occurred.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``oldShardKey`

</td>
<td headers="Type">
Document

</td>
<td headers="Description">
The [shard key](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/sharding-shard-key/#std-label-shard-key) for the collection that changed.

</td>
</tr>
</table>

## Example

The following example shows a `refineCollectionShardKey` event:

```json
{
   "_id": { <ResumeToken> },
   "operationType": "refineCollectionShardKey",
   "clusterTime": Timestamp({ t: 1654894852, i: 52 }),
   "collectionUUID": UUID("98046a1a-b649-4e5b-9c75-67594221ce19"),
   "ns": {"db": "reshard_collection_event", "coll": "coll"},
   "operationDescription": {
     "shardKey": {"_id": 1, akey: 1},
     "oldShardKey": {"_id": 1}
   }
}
```


# reshardCollection Event

ce-reshardCollection## Summary

`reshardCollection`
A `reshardCollection` event occurs when:

- The shard key for a collection and the distribution of your data is changed, and

- The change stream has [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) set to `true`.

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
`operationDescription.``reshardUUID`

</td>
<td headers="Type">
UUID

</td>
<td headers="Description">
UUID (Universally Unique Identifier) that identifies the resharding operation.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.` | `shardKey`

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
`operationDescription.` | `oldShardKey`

</td>
<td headers="Type">
Document

</td>
<td headers="Description">
The [shard key](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/sharding-shard-key/#std-label-shard-key) for the collection that changed.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.` | `unique`

</td>
<td headers="Type">
Boolean

</td>
<td headers="Description">
This has a value of true if the collection was sharded with a unique shard key.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.` | `numInitialChunks`

</td>
<td headers="Type">
NumberLong

</td>
<td headers="Description">
Number of chunks created on each shard during a `reshardCollection` operation.

Starting in MongoDB 8.2, resharding operations ignore the `numInitialChunks` setting when the shard key contains a hashed prefix. Instead, MongoDB deterministically splits the hashed key space among recipients, using the same approach as initial chunk creation for empty hashed collections.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``collation`

</td>
<td headers="Type">
Document

</td>
<td headers="Description">
[Collation](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/collation/#std-label-collation) document used for the shard key index.

</td>
</tr>
<tr>
<td headers="Field">
`operationDescription.``zones`

</td>
<td headers="Type">
Array

</td>
<td headers="Description">
The zones added for the new shard key.

</td>
</tr>
</table>

## Example

The following example shows a `reshardCollection` event:

```json
{
   "_id": { <ResumeToken> },
   "operationType": "reshardCollection",
   "collectionUUID": 0,
   "ns": {"db": "reshard_collection_event", "coll": "coll"},
   "operationDescription": {
     "reshardUUID": 0,
     "shardKey": {"newKey": 1},
     "oldShardKey": {"_id": 1},
     "unique": false,
     "numInitialChunks": Long(1),
     "collation": {"locale": "simple"},
     "zones": [
         {"zone": "zone1", "min": {"newKey": {"$minKey": 1}}, "max": {"newKey": {"$maxKey": 1}}}
     ]
   }
}
```


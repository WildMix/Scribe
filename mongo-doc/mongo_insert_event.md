# insert Event

ce-insert`collectionUUID`[updateDescription.disambiguatedPaths](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-ud-disambiguatedPaths)## Summary

`insert`
An `insert` event occurs when an operation adds documents to a collection.

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

In MongoDB 8.2.0, `collectionUUID` and [updateDescription.disambiguatedPaths](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-ud-disambiguatedPaths) are included in applicable change events even if you do not set `showExpandedEvents`. In MongoDB versions earlier than 8.2.0 and in versions 8.2.1 and later, these fields are included only if you open the change stream with `showExpandedEvents: true`.

</td>
</tr>
<tr>
<td headers="Field">
`documentKey`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
Document that contains the `_id` value of the document created or modified by the [CRUD](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/crud/#std-label-crud) operation.

For sharded collections, this field also displays the full shard key for the document. The `_id` field is not repeated if it is already a part of the shard key.

</td>
</tr>
<tr>
<td headers="Field">
`fullDocument`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
The document created by the operation.

Starting in MongoDB 6.0, if you set the `changeStreamPreAndPostImages` option using [`db.createCollection()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.createCollection/#mongodb-method-db.createCollection), [`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/create/#mongodb-dbcommand-dbcmd.create), or [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod), then the `fullDocument` field shows the document after it was inserted, replaced, or updated (the document post-image). `fullDocument` is always included for `insert` events.

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
`operationType`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The type of operation that the change notification reports.

Returns a value of `insert` for these change events.

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
Together with the [lsid](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/insert/#std-label--idref--lsid), a number that helps uniquely identify a transction.

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

## Behavior

### Update Operations

The [`update`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/update/#mongodb-dbcommand-dbcmd.update) command can produce different change events (not just [`update`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#mongodb-data-update)) depending on the actual changes it makes to the collection.

<table>
<tr>
<th id="Change%20Event">
Change Event

</th>
<th id="Description">
Description

</th>
</tr>
<tr>
<td headers="Change%20Event">
[`update`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#mongodb-data-update)

</td>
<td headers="Description">
The update operation modified an existing document.

</td>
</tr>
<tr>
<td headers="Change%20Event">
[`replace`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/replace/#mongodb-data-replace)

</td>
<td headers="Description">
The update operation replaced the document or produced a diff that was more verbose than the original document, causing MongoDB to replace it.

</td>
</tr>
<tr>
<td headers="Change%20Event">
[`insert`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/insert/#mongodb-data-insert)

</td>
<td headers="Description">
The update operation attempted to update a document that doesn't exist and instead added the document to the collection. This only occurs when the update runs with the `upsert` option enabled.

</td>
</tr>
</table>

## Example

The following example illustrates an `insert` event:

```json
{
   "_id": { <Resume Token> },
   "operationType": "insert",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>,
   "ns": {
      "db": "engineering",
      "coll": "users"
   },
   "documentKey": {
      "userName": "alice123",
      "_id": ObjectId("599af247bb69cd89961c986d")
   },
   "fullDocument": {
      "_id": ObjectId("599af247bb69cd89961c986d"),
      "userName": "alice123",
      "name": "Alice"
   }
}
```

The `documentKey` field includes both the `_id` and the `userName` field. This indicates that the `engineering.users` collection is sharded, with a shard key on `userName` and `_id`.

The `fullDocument` document represents the version of the document at the time of the insert.


# delete Event

ce-delete`collectionUUID`[updateDescription.disambiguatedPaths](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-ud-disambiguatedPaths)## Synopsis

`delete`
A `delete` event occurs when operations remove documents from a collection, such as when a user or application executes the [`delete`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/delete/#mongodb-dbcommand-dbcmd.delete) command.

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
`operationType`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The type of operation that the change notification reports.

Returns a value of `delete` for these change events.

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
Together with the [lsid](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/delete/#std-label--idref--lsid), a number that helps uniquely identify a transction.

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

### Document Pre- and Post-Images

Starting in MongoDB 6.0, you see a `fullDocumentBeforeChange` document with the fields before the document was changed (or deleted) if you perform these steps:

1. Enable the new `changeStreamPreAndPostImages` field for a collection using [`db.createCollection()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.createCollection/#mongodb-method-db.createCollection), [`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/create/#mongodb-dbcommand-dbcmd.create), or [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod).

2. Set `fullDocumentBeforeChange` to `"required"` or `"whenAvailable"` in [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch).

Example `fullDocumentBeforeChange` document in the change stream output:

```json
"fullDocumentBeforeChange" : {
   "_id" : ObjectId("599af247bb69cd89961c986d"),
   "userName" : "alice123",
   "name" : "Alice Smith"
}
```

For complete examples with the change stream output, see [Change Streams with Document Pre- and Post-Images](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#std-label-db.collection.watch-change-streams-pre-and-post-images-example).

Pre- and post-images are not available for a [change stream event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) if the images were:

- Not enabled on the collection at the time of a document update or delete operation.

- Removed after the pre- and post-image retention time set in `expireAfterSeconds`.

    - The following example sets `expireAfterSeconds` to `100` seconds on an entire cluster:

      ```javascript
      use admin
      db.runCommand( {
         setClusterParameter:
            { changeStreamOptions: {
               preAndPostImages: { expireAfterSeconds: 100 }
            } }
      } )
      ```

      The [`setClusterParameter`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/setClusterParameter/#mongodb-dbcommand-dbcmd.setClusterParameter) command is not supported in MongoDB Atlas clusters. For information on Atlas support for all commands, see [Unsupported Commands in Atlas](https://www.mongodb.com/docs/atlas/unsupported-commands/#std-label-unsupported-commands).

    - The following example returns the current `changeStreamOptions` settings, including `expireAfterSeconds`:

      ```javascript
      db.adminCommand( { getClusterParameter: "changeStreamOptions" } )
      ```

    - Setting `expireAfterSeconds` to `off` uses the default retention policy: pre- and post-images are retained until the corresponding change stream events are removed from the [oplog](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-oplog).

    - If a change stream event is removed from the oplog, then the corresponding pre- and post-images are also deleted regardless of the `expireAfterSeconds` pre- and post-image retention time.

Additional considerations:

- Enabling pre- and post-images consumes storage space and adds processing time. Only enable pre- and post-images if you need them.

- Limit the change stream event size to less than 16 mebibytes. To limit the event size, you can:

    - Limit the document size to 8 megabytes. You can request pre- and post-images simultaneously in the [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) if other change stream event fields like `updateDescription` are not large.

    - Request only post-images in the change stream output for documents up to 16 mebibytes if other change stream event fields like `updateDescription` are not large.

    - Request only pre-images in the change stream output for documents up to 16 mebibytes if:

        - document updates affect only a small fraction of the document structure or content, *and*

        - do not cause a `replace` change event. A `replace` event always includes the post-image.

- To request a pre-image, you set `fullDocumentBeforeChange` to `required` or `whenAvailable` in [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch). To request a post-image, you set `fullDocument` using the same method.

- Pre-images are written to the [`config.system.preimages`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/system-collections/#mongodb-data-config.system.preimages) collection.

    - The `config.system.preimages` collection may become large. To limit the collection size, you can set `expireAfterSeconds` time for the pre-images as shown earlier.

    - To monitor the size of `config.system.preimages`, connect to a shard node on a sharded cluster or a [`mongod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/program/mongod/#mongodb-binary-bin.mongod) node on a replica set. Then, run the following commands:

      ```javascript
      use config
      db.system.preimages.totalSize()
      db.system.preimages.stats()
      ```

    - Pre-images are removed asynchronously by a background process.

Starting in MongoDB 6.0, if you are using document pre- and post-images for [change streams](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output), you must disable [changeStreamPreAndPostImages](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#std-label-collMod-change-stream-pre-and-post-images) for each collection using the [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod) command before you can downgrade to an earlier MongoDB version.

- For change stream events and output, see [Change Events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output).

- To watch a collection for changes, see [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch).

- For complete examples with the change stream output, see [Change Streams with Document Pre- and Post-Images](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#std-label-db.collection.watch-change-streams-pre-and-post-images-example).

## Example

The following example illustrates a `delete` event:

```json
{
   "_id": { <Resume Token> },
   "operationType": "delete",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>,
   "ns": {
      "db": "engineering",
      "coll": "users"
   },
   "documentKey": {
      "_id": ObjectId("599af247bb69cd89961c986d")
   }
}
```

The `fullDocument` document is omitted as the document no longer exists at the time the change stream cursor sends the `delete` event to the client.


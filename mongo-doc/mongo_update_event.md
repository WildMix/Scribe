# `update` Event

ce-update`collectionUUID``updateDescription.disambiguatedPaths`## Summary

`update`
An `update` event occurs when an operation updates a document in a collection.

To learn more about events that occur when collection options are modified, see the [`modify`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/modify/#mongodb-data-modify) event.

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

In MongoDB 8.2.0, `collectionUUID` and `updateDescription.disambiguatedPaths` are included in applicable change events even if you do not set `showExpandedEvents`. In MongoDB versions earlier than 8.2.0 and in versions 8.2.1 and later, these fields are included only if you open the change stream with `showExpandedEvents: true`.

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
The document created or modified by a [CRUD](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-CRUD) operation.

This field only appears if you configured the change stream with `fullDocument` set to `updateLookup`. When you configure the change stream with `updateLookup`, the field represents the current majority-committed version of the document modified by the update operation. The document may differ from the changes described in [updateDescription](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label--idref--updateDescription) if any other majority-committed operations have modified the document between the original update operation and the full document lookup.

For more information, see [Lookup Full Document for Update Operations](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-streams-updateLookup).

Starting in MongoDB 6.0, if you set the `changeStreamPreAndPostImages` option using [`db.createCollection()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.createCollection/#mongodb-method-db.createCollection), [`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/create/#mongodb-dbcommand-dbcmd.create), or [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod), then the `fullDocument` field shows the document after it was inserted, replaced, or updated (the document post-image). `fullDocument` is always included for `insert` events.

</td>
</tr>
<tr>
<td headers="Field">
`fullDocumentBeforeChange`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
The document before changes were applied by the operation. That is, the document pre-image.

This field is available when you enable the `changeStreamPreAndPostImages` field for a collection using [`db.createCollection()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.createCollection/#mongodb-method-db.createCollection) method or the [`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/create/#mongodb-dbcommand-dbcmd.create) or [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod) commands.

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

Returns a value of `update` for these change events.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
A document describing the fields that were updated or removed by the update operation.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``disambiguatedPaths`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
A document that provides clarification of ambiguous field descriptors in `updateDescription`.

When the `update` change event describes changes on a field where the path contains a period (`.`) or where the path includes a non-array numeric subfield, the `disambiguatedPath` field provides a document with an array that lists each entry in the path to the modified field.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

In MongoDB 8.2.0, `collectionUUID` and `updateDescription.disambiguatedPaths` are included in applicable change events even if you do not set `showExpandedEvents`. In MongoDB versions earlier than 8.2.0 and in versions 8.2.1 and later, these fields are included only if you open the change stream with `showExpandedEvents: true`.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``removedFields`

</td>
<td headers="Type">
array

</td>
<td headers="Description">
An array of fields that were removed by the update operation.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``truncatedArrays`

</td>
<td headers="Type">
array

</td>
<td headers="Description">
An array of documents which record array truncations performed with pipeline-based updates using one or more of the following stages:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

If the entire array is replaced, the truncations will be reported under [updateDescription.updatedFields](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label--idref--ud-updatedFields).

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``truncatedArrays.``field`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The name of the truncated field.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``truncatedArrays.``newSize`

</td>
<td headers="Type">
integer

</td>
<td headers="Description">
The number of elements in the truncated array.

</td>
</tr>
<tr>
<td headers="Field">
`updateDescription.``updatedFields`

</td>
<td headers="Type">
document

</td>
<td headers="Description">
A document whose keys correspond to the fields that were modified by the update operation. The value of each field corresponds to the new value of those fields, rather than the operation that resulted in the new value.

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
Together with the [lsid](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label--idref--lsid), a number that helps uniquely identify a transction.

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

### Path Disambiguation

The `updateDescription` field notes changes made to specific fields in documents by an operation.  These field descriptors use dots (`.`) as path separators and numbers as array indexes, which leads to some ambiguity when it contains field names that use dots or numbers.

When an `update` event reports changes involving ambiguous fields, the `disambiguatedPaths` document provides the path key with an array listing each path component.

The `disambiguatedPaths` field is available for change streams that include expanded events.

In MongoDB 8.2.0, `collectionUUID` and `updateDescription.disambiguatedPaths` are included in applicable change events even if you do not set `showExpandedEvents`. In MongoDB versions earlier than 8.2.0 and in versions 8.2.1 and later, these fields are included only if you open the change stream with `showExpandedEvents: true`.

For example, consider a document that lists people and the towns in which they live:

```json
{
   "name": "Anthony Trollope",
   "home.town": "Oxford",
   "residences": [
      {"0": "Oxford"},
      {"1": "Sunbury"}
   ]
}
```

- When an update modifies the `home.town` field from `Oxford` to `London`, it produces an update description that looks like this:

  ```json
  "updateDescription": {
     "updatedFields": {
        "home.town": "London"
     },
     "disambiguatedPaths": {
        "home.town": [ "home.town" ]
     }
  }
  ```

  Because the field `home.town` contains a period, the `disambiguatedPaths` field shows an array with one value, to indicate that `town` is not a sub-field of `home`.

- When an update modifies a value in the `residences` array to make the same change, it produces an update description that looks like this:

  ```json
  "updateDescription": {
     "updatedFields": {
        "residences.0.0": "London"
     },
     "disambiguatedPaths": { "residences.0.0": [ "residences", 0, "0" ] }
  }
  ```

  The disambiguated paths include an integer `0` to indicate the array index and the string `"0"` to indicate the field name within the nested document.

There are two cases where `disambiguatedPath` does **not** include a numeric field:

- When the first field in the path is a numeric string (i.e. `0.name`). This is not ambiguous since the first field cannot be an array index.

- When the numeric string field has leading zeroes (i.e., `0001`). This is not ambiguous since an integer cannot have leading zeroes.

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

### Comparison of Standard Updates and Pipeline Updates

In some scenarios, standard updates and [pipeline updates](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/tutorial/update-documents-with-aggregation-pipeline/#std-label-updates-agg-pipeline) produce different change events for the same logical change. If you switch between standard and pipeline update syntax, you may observe unexpected change events in your application.

The following example shows how the different update syntaxes produce different change events when removing subfields from embedded documents. This is only one of many scenarios where change events differ based on the update syntax used.

- **Standard Updates:** When subfields of an embedded object are removed, the removed paths are always in the `removedFields` array.

- **Pipeline Updates:** When subfields of an embedded object are removed, the resulting change event shows the most concise description of the update. For example:

    - If a small number of subfields are removed (for example, removing 1 out of 3 subfields), the change is shown in the `removedFields` array.

    - If most subfields are removed (for example, removing 2 out of 3 subfields), the change is shown in the `updatedFields` array, and the parent object reflects the new state.

The following example script shows the different change events generated by standard updates and pipeline updates that remove subfields of an object:

```javascript
// 1. Setup collection and insert documents for test cases

let contactMethods = {
   email: "user@example.com",
   phone: "555-0100",
   mobile: "555-0101"
};

db.cstest.drop();
db.cstest.insertMany([

   // Case 1: Remove 1 of 3 subfields
   { _id: "standard_remove_1_of_3", contactMethods: contactMethods },
   { _id: "pipeline_remove_1_of_3", contactMethods: contactMethods },

   // Case 2: Remove 2 of 3 subfields
   { _id: "standard_remove_2_of_3", contactMethods: contactMethods },
   { _id: "pipeline_remove_2_of_3", contactMethods: contactMethods }
]);

// 2. Open a Change Stream
const csCursor = db.cstest.watch();

// --- CASE 1: Remove 1 out of 3 subfields ---
db.cstest.updateOne({ _id: "standard_remove_1_of_3" }, { $unset: { "contactMethods.email": "" } });
db.cstest.updateOne({ _id: "pipeline_remove_1_of_3" }, [{ $unset: "contactMethods.email" }]);

// --- CASE 2: Remove 2 out of 3 subfields ---
db.cstest.updateOne({ _id: "standard_remove_2_of_3" }, { $unset: { "contactMethods.email": "", "contactMethods.phone": "" } });
db.cstest.updateOne({ _id: "pipeline_remove_2_of_3" }, [{ $unset: ["contactMethods.email", "contactMethods.phone"] }]);

// 3. Print the change stream updateDescriptions
print("\n--- Change Stream Results ---");
for (let i = 0; i < 4; i++) {
   if (csCursor.hasNext()) {
      let event = csCursor.next();
      print(`\nOperation: ${event.documentKey._id}`);
      print(JSON.stringify(event.updateDescription));
   }
}
csCursor.close();
```

```javascript
--- Change Stream Results ---

Operation: standard_remove_1_of_3
{"updatedFields":{},"removedFields":["contactMethods.email"],"truncatedArrays":[]}

Operation: pipeline_remove_1_of_3
{"updatedFields":{},"removedFields":["contactMethods.email"],"truncatedArrays":[]}

Operation: standard_remove_2_of_3
{"updatedFields":{},"removedFields":["contactMethods.email","contactMethods.phone"],"truncatedArrays":[]}

Operation: pipeline_remove_2_of_3
{"updatedFields":{"contactMethods":{"mobile":"555-0101"}},"removedFields":[],"truncatedArrays":[]}
```

### Array Updates

Updates to arrays produce `update` change events, but the `updateDescription.updateFields` can show different values.

For example, consider the following document and updates:

```javascript
db.students.insertOne( { student_id: 1, scores: [ ] } )

db.students.updateOne(
   { student_id: 1 },
   { $push: { scores: 0.85 } }
)

db.students.updateOne(
   { student_id: 1 },
   { $push: { scores: 0.94 } }
)

db.students.updateOne(
   { student_id: 1 },
   { $pull: { scores: 0.94 } }
)
```

The first update operates on an empty array. Here, the [`$push`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/update/push/#mongodb-update-up.-push) produces an `update` change event where the field is replaced by single-entry array with the given value:

```javascript
{
   _id: { _data: '82642AD66B000000012B022C0100296E5A10045DC4B11BEA5F4319A8E7CAF46816ED71461E5F6964002B060004' },
   operationType: 'update',
   clusterTime: Timestamp({ t: 1680529003, i: 1 }),
   ns: { db: 'communication_chat', coll: 'students' },
   documentKey: { student_id: 1 },
   updateDescription: {
      updatedFields: { scores: [ 0.85 ] },
      removedFields: [],
      truncatedArrays: []
   }
}
```

In the second update operation, the array now contains values. The [`$push`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/update/push/#mongodb-update-up.-push) adds a new entry in the array. The `update` change event then shows it as a change on the new position in the array (that is, `scores.1`):

```javascript
{
  _id: { _data: '82642AD673000000012B022C0100296E5A10045DC4B11BEA5F4319A8E7CAF46816ED71461E5F6964002B060004' },
  operationType: 'update',
  clusterTime: Timestamp({ t: 1680529011, i: 1 }),
  ns: { db: 'communication_chat', coll: 'students' },
  documentKey: { student_id: 1 },
  updateDescription: {
    updatedFields: { 'scores.1': 0.94 },
    removedFields: [],
    truncatedArrays: []
  }
}
```

If you run the update operation again to add a third score to the student's record, it would produce an `update` change event that modifies `scores.2`.

Removal of array items with the [`$pull`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/update/pull/#mongodb-update-up.-pull) operator produces a change event that shows the new array:

```javascript
{
  _id: { _data: '82642AD673000000012B022C0100296E5A10045DC4B11BEA5F4319A8E7CAF46816ED71461E5F6964002B060004' },
  operationType: 'update',
  clusterTime: Timestamp({ t: 1680529011, i: 1 }),
  ns: { db: 'communication_chat', coll: 'students' },
  documentKey: { student_id: 1 },
  updateDescription: {
    updatedFields: { scores: [ 0.85 ] },
    removedFields: [],
    truncatedArrays: []
  }
}
```

### Array Truncation

Update operations that reduce the number of elements in arrays through the [pipeline updates](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/tutorial/update-documents-with-aggregation-pipeline/#std-label-updates-agg-pipeline), such as with the [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields) or [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set) aggregation stages, show the updated array and new size in the `truncatedArrays` field.

```javascript
db.students.insertOne( { student_id: 2, scores: [ 0.85, 0.94, 0.78 ] } )

db.students.updateOne(
   { student_id: 2 },
   [ { $addFields: { scores: [ 0.85, 0.94 ] } } ]
)

db.students.updateOne(
   { student_id: 2 },
   [ { $addFields: { scores: [ 0.85, 0.94, 0.78 ] } } ]
)
```

The change event from the first update, which used the [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields) stage to remove a value from the `scores` field shows the change in the `truncatedArrays` field:

```javascript
{
  _id: { _data: '82642AD673000000012B022C0100296E5A10045DC4B11BEA5F4319A8E7CAF46816ED71461E5F6964002B060004' },
  operationType: 'update',
  clusterTime: Timestamp({ t: 1680529011, i: 1 }),
  ns: { db: 'communication_chat', coll: 'students' },
  documentKey: { student_id: 2 },
  updateDescription: {
    updatedFields: {},
    removedFields: [],
    truncatedArrays: [ { fields: "scores", newSize: 2 } ]
  }
}
```

The change event from the second update, which used the [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields) stage to add a value back to the `scores` field, shows the update in the `updatedFields` field:

```javascript
{
  _id: { _data: '82642AD673000000012B022C0100296E5A10045DC4B11BEA5F4319A8E7CAF46816ED71461E5F6964002B060004' },
  operationType: 'update',
  clusterTime: Timestamp({ t: 1680529011, i: 1 }),
  ns: { db: 'communication_chat', coll: 'students' },
  documentKey: { student_id: 2 },
  updateDescription: {
    updatedFields: { scores.2: 0.78 },
    removedFields: [],
    truncatedArrays: []
  }
}
```

## Example

The following example illustrates an `update` event:

```json
{
   "_id": { <Resume Token> },
   "operationType": "update",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>,
   "ns": {
      "db": "engineering",
      "coll": "users"
   },
   "documentKey": {
      "_id": ObjectId("58a4eb4a30c75625e00d2820")
   },
   "updateDescription": {
      "updatedFields": {
         "email": "alice@10gen.com"
      },
      "removedFields": ["phoneNumber"],
      "truncatedArrays": [ {
         "field" : "vacation_time",
         "newSize" : 36
      } ]
   }
}
```

The following example illustrates an `update` event for change streams opened with the `fullDocument : updateLookup` option:

```json
{
   "_id": { <Resume Token> },
   "operationType": "update",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>,
   "ns": {
      "db": "engineering",
      "coll": "users"
   },
   "documentKey": {
      "_id": ObjectId("58a4eb4a30c75625e00d2820")
   },
   "updateDescription": {
      "updatedFields": {
         "email": "alice@10gen.com"
      },
      "removedFields": ["phoneNumber"],
      "truncatedArrays": [ {
         "field" : "vacation_time",
         "newSize" : 36
      } ],
      "disambiguatedPaths": { }
   },
   "fullDocument": {
      "_id": ObjectId("58a4eb4a30c75625e00d2820"),
      "name": "Alice",
      "userName": "alice123",
      "email": "alice@10gen.com",
      "team": "replication"
   }
}
```

The `fullDocument` document represents the most current majority-committed version of the updated document. The `fullDocument` document may vary from the document at the time of the update operation depending on the number of interleaving majority-committed operations that occur between the update operation and the document lookup.


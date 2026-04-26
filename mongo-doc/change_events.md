# Change Events

[collectionUUID](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-collectionUUID)[updateDescription.disambiguatedPaths](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-ud-disambiguatedPaths)Change streams watch collections, databases, or deployments for changes.

When a change occurs on a watched resource, the change stream returns a change event notification document, with information on the operation and the changes it made.

## Operation Types

<table>
<tr>
<th id="Event">
Event

</th>
<th id="Description">
Description

</th>
</tr>
<tr>
<td headers="Event">
[`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/create/#mongodb-data-create)

</td>
<td headers="Description">
Occurs on the creation of a collection.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`createIndexes`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/createIndexes/#mongodb-data-createIndexes)

</td>
<td headers="Description">
Occurs on the creation of indexes on the collection.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`delete`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/delete/#mongodb-data-delete)

</td>
<td headers="Description">
Occurs when a document is removed from the collection.

</td>
</tr>
<tr>
<td headers="Event">
[`drop`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/drop/#mongodb-data-drop)

</td>
<td headers="Description">
Occurs when a collection is dropped from a database.

</td>
</tr>
<tr>
<td headers="Event">
[`dropDatabase`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropDatabase/#mongodb-data-dropDatabase)

</td>
<td headers="Description">
Occurs when a database is dropped.

</td>
</tr>
<tr>
<td headers="Event">
[`dropIndexes`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropIndexes/#mongodb-data-dropIndexes)

</td>
<td headers="Description">
Occurs when an index is dropped from the collection.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`insert`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/insert/#mongodb-data-insert)

</td>
<td headers="Description">
Occurs when an operation adds documents to a collection.

</td>
</tr>
<tr>
<td headers="Event">
[`invalidate`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#mongodb-data-invalidate)

</td>
<td headers="Description">
Occurs when an operation renders the change stream invalid.

</td>
</tr>
<tr>
<td headers="Event">
[`modify`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/modify/#mongodb-data-modify)

</td>
<td headers="Description">
Occurs when a collection is modified.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`refineCollectionShardKey`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/refineCollectionShardKey/#mongodb-data-refineCollectionShardKey)

</td>
<td headers="Description">
Occurs when a shard key is modified.

</td>
</tr>
<tr>
<td headers="Event">
[`rename`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/rename/#mongodb-data-rename)

</td>
<td headers="Description">
Occurs when a collection is renamed.

</td>
</tr>
<tr>
<td headers="Event">
[`replace`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/replace/#mongodb-data-replace)

</td>
<td headers="Description">
Occurs when an update operation removes a document from a collection and replaces it with a new document.

</td>
</tr>
<tr>
<td headers="Event">
[`reshardCollection`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/reshardCollection/#mongodb-data-reshardCollection)

</td>
<td headers="Description">
Occurs when the shard key for a collection and the distribution of data changes.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`shardCollection`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/shardCollection/#mongodb-data-shardCollection)

</td>
<td headers="Description">
Occurs when a collection is sharded.

Requires that you set the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option to `true`.

</td>
</tr>
<tr>
<td headers="Event">
[`update`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#mongodb-data-update)

</td>
<td headers="Description">
Occurs when an operation updates a document in a collection.

</td>
</tr>
</table>The server might internally process and return update operations as replace operations if the representation of replace operations are more concise. If you are listening for update operations, we strongly recommend also listening for replace operations.

## Resume Token

Each change event includes an `_id` field, which is a [BSON](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-BSON) object that serves as an identifier for the change stream event. For an example of resuming a change stream by `resumeToken`, see [Resume a Change Stream](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume).

## Expanded Events

Starting in MongoDB 6.0, change streams support change notifications for DDL events, like the [createIndexes](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/createIndexes/#std-label-change-event-createIndexes) and [dropIndexes](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropIndexes/#std-label-change-event-dropIndexes) events. To include expanded events in a change stream, create the change stream cursor using the `showExpandedEvents` option.

In MongoDB 8.2.0, [collectionUUID](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-collectionUUID) and [updateDescription.disambiguatedPaths](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/update/#std-label-ce-update-ud-disambiguatedPaths) are included in applicable change events even if you do not set `showExpandedEvents`. In MongoDB versions earlier than 8.2.0 and in versions 8.2.1 and later, these fields are included only if you open the change stream with `showExpandedEvents: true`.

For example:

```javascript
let cur = db.names.aggregate( [ {
   $changeStream: {
       showExpandedEvents: true
     }
   }
 ] )

cur.next()
```


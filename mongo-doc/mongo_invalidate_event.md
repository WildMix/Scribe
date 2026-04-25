# invalidate Event

ce-invalidate## Summary

`invalidate`
An `invalidate` event occurs when an operation renders the change stream invalid. For example, a change stream opened on a collection that was later dropped or renamed would cause an `invalidate` event.

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
`operationType`

</td>
<td headers="Type">
string

</td>
<td headers="Description">
The type of operation that the change notification reports.

Returns a value of `invalidate` for these change events.

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

The following example illustrates an `invalidate` event:

```json
{
   "_id": { <Resume Token> },
   "operationType": "invalidate",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>
}
```

Change streams opened on collections raise an `invalidate` event when a [drop](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/drop/#std-label-change-event-drop), [rename](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/rename/#std-label-change-event-rename), or [dropDatabase](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropDatabase/#std-label-change-event-dropDatabase) operation occurs that affects the watched collection.

Change streams opened on databases raise an `invalidate` event when a [dropDatabase](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropDatabase/#std-label-change-event-dropDatabase) event occurs that affects the watched database.

`invalidate` events close the change stream cursor.

You cannot use `resumeAfter` to resume a change stream after an [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate) (for example, a collection drop or rename) closes the stream. Instead, you can use [startAfter](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-start-after) to start a new change stream after an [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate).


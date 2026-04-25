# dropDatabase Event

ce-dropDatabase## Synopsis

`dropDatabase`
A `dropDatabase` event occurs when a database is dropped.

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

Returns a value of `dropDatabase` for these change events.

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
Together with the [lsid](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/dropDatabase/#std-label--idref--lsid), a number that helps uniquely identify a transction.

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

The following example illustrates a `dropDatabase` event:

```json
{
   "_id": { <Resume Token> },
   "operationType": "dropDatabase",
   "clusterTime": <Timestamp>,
   "wallTime": <ISODate>,
   "ns": {
      "db": "engineering"
   }
}
```

A [`dropDatabase`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/dropDatabase/#mongodb-dbcommand-dbcmd.dropDatabase) command generates a [drop event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/drop/#std-label-change-streams-drop-event) for each collection in the database before generating a `dropDatabase` event for the database.

A `dropDatabase` event leads to an [`invalidate`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#mongodb-data-invalidate) event for change streams opened against its own `ns.db` database.


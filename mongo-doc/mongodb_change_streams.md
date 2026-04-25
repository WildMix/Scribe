# Change Streams

Change streams allow applications to access real-time data changes without the prior complexity and risk of manually tailing the [oplog](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-oplog). Applications can use change streams to subscribe to all data changes on a single collection, a database, or an entire deployment, and immediately react to them. Because change streams use the aggregation framework, applications can also filter for specific changes or transform the notifications at will.

Change streams are restricted to database events. Atlas Stream Processing has extended functionality, including managing multiple data event types and processing streams of complex data using the same query API as Atlas databases. For more information, see [Atlas Stream Processing](https://www.mongodb.com/docs/atlas/atlas-stream-processing/#std-label-atlas-sp).

change streamsStarting in MongoDB 5.1, change streams are optimized, providing more efficient resource utilization and faster execution of some aggregation pipeline stages.

## Availability

Change streams are available for [replica sets](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/replication/#std-label-replication) and [sharded clusters](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/sharding/#std-label-sharding-background):

- **Storage Engine.**

  The replica sets and sharded clusters must use the [WiredTiger](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/wiredtiger/#std-label-storage-wiredtiger) storage engine. Change streams can also be used on deployments that employ MongoDB's [encryption-at-rest](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/security-encryption-at-rest/#std-label-encrypted-storage-engine) feature.

- **Replica Set Protocol Version.**

  The replica sets and sharded clusters must use replica set protocol version 1 ([`pv1`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/replica-configuration/#mongodb-rsconf-rsconf.protocolVersion)).

- **Read Concern "majority" Enablement.**

  [Change streams](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-changeStreams) are available regardless of the [`"majority"`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/read-concern-majority/#mongodb-readconcern-readconcern.-majority-) read concern support; that is, read concern `majority` support can be either enabled (default) or [disabled](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/read-concern-majority/#std-label-disable-read-concern-majority) to use change streams.

Time series collections do not support change streams because time series collections use an optimized storage format instead of tracking changes at the document level. You cannot use time series collections as a source for Atlas Stream Processing.

See [Time Series Collection Limitations](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/timeseries/timeseries-limitations/#std-label-manual-timeseries-collection-limitations) for more information.

### Stable API Support

Change streams are included in [Stable API](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/stable-api/#std-label-stable-api) V1. However, the [showExpandedEvents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-streams-expanded-events) option is not included in Stable API V1.

## Connect

Connections for a change stream can either use DNS seed lists with the `+srv` connection option or by listing the servers individually in the connection string.

If the driver loses the connection to a change stream or the connection goes down, it attempts to reestablish a connection to the change stream through another node in the cluster that has a matching [read preference](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/connection-string-options/#std-label-connections-read-preference). If the driver cannot find a node with the correct read preference, it throws an exception.

For more information, see [Connection String URI Format](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/connection-string/#std-label-mongodb-uri).

## Watch a Collection, Database, or Deployment

You can open change streams against:

<table>
<tr>
<th id="Target">
Target

</th>
<th id="Description">
Description

</th>
</tr>
<tr>
<td headers="Target">
A collection

</td>
<td headers="Description">
You can open a change stream cursor for a single collection (except `system` collections, or any collections in the `admin`, `local`, and `config` databases).

The examples on this page use the MongoDB drivers to open and work with a change stream cursor for a single collection. See also the [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh) method [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch).

</td>
</tr>
<tr>
<td headers="Target">
A database

</td>
<td headers="Description">
You can open a change stream cursor for a single database (excluding `admin`, `local`, and `config` database) to watch for changes to all its non-system collections.

For the MongoDB driver method, refer to your driver documentation. See also the [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh) method [`db.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.watch/#mongodb-method-db.watch).

</td>
</tr>
<tr>
<td headers="Target">
A deployment

</td>
<td headers="Description">
You can open a change stream cursor for a deployment (either a replica set or a sharded cluster) to watch for changes to all non-system collections across all databases except for `admin`, `local`, and `config`.

For the MongoDB driver method, refer to your driver documentation. See also the [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh) method [`Mongo.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/Mongo.watch/#mongodb-method-Mongo.watch).

</td>
</tr>
</table>The examples on this page use the MongoDB drivers to illustrate how to open a change stream cursor for a collection and work with the change stream cursor.

## Change Stream Performance Considerations

If the amount of active change streams opened against a database exceeds the [connection pool size](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/administration/connection-pool-overview/#std-label-connection-pool-overview), you may experience notification latency. Each change stream uses a connection and a [getMore](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/getMore/#std-label-manual-reference-commands-getMore) operation on the change stream for the period of time that it waits for the next event. To avoid any latency issues, you should ensure that the pool size is greater than the number of opened change streams. For details see the [maxPoolSize](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/administration/connection-pool-overview/#std-label-maxpoolsize-cp-setting) setting.

### Sharded Cluster Considerations

When a change stream is opened on a sharded cluster:

- The [`mongos`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/program/mongos/#mongodb-binary-bin.mongos) creates individual change streams on **each shard**. This behavior occurs regardless of whether the change stream targets a particular shard key range.

- When the `mongos` receives change stream results, it sorts and filters those results. If needed, the `mongos` also performs a `fullDocument` lookup.

For best performance, limit the use of [`$lookup`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/lookup/#mongodb-pipeline-pipe.-lookup) queries in change streams.

## Open A Change Stream

To open a change stream:

- For a replica set, you can issue the open change stream operation from any of the data-bearing members.

- For a sharded cluster, you must issue the open change stream operation from the [`mongos`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/program/mongos/#mongodb-binary-bin.mongos).

The following example opens a change stream for a collection and iterates over the cursor to retrieve the change stream documents.

➤➤ Use the **Select your language** drop-down menu in the upper-right to set the language of the examples on this page.

<Tabs>

<Tab name="C">

The C examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://www.mongodb.com/docs/languages/c/c-driver/current/databases-collections/#access-a-database) that contains an `inventory` collection.

```c
mongoc_collection_t *collection;
bson_t *pipeline = bson_new ();
bson_t opts = BSON_INITIALIZER;
mongoc_change_stream_t *stream;
const bson_t *change;
const bson_t *resume_token;
bson_error_t error;

collection = mongoc_database_get_collection (db, "inventory");
stream = mongoc_collection_watch (collection, pipeline, NULL /* opts */);
mongoc_change_stream_next (stream, &change);
if (mongoc_change_stream_error_document (stream, &error, NULL)) {
   MONGOC_ERROR ("%s\n", error.message);
}

mongoc_change_stream_destroy (stream);
```

</Tab>

<Tab name="C#">

The C# examples below assume that you have [connected to a MongoDB replica set and have accessed a database](http://mongodb.github.io/mongo-csharp-driver/2.4/getting_started/quick_tour/#make-a-connection/) that contains an `inventory` collection.

```csharp
var cursor = inventory.Watch();
while (cursor.MoveNext() && cursor.Current.Count() == 0) { } // keep calling MoveNext until we've read the first batch
var next = cursor.Current.First();
cursor.Dispose();
```

</Tab>

<Tab name="Go">

The Go examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://godoc.org/go.mongodb.org/mongo-driver/mongo#NewClient/) that contains an `inventory` collection.

```go

cs, err := coll.Watch(ctx, mongo.Pipeline{})
assert.NoError(t, err)
defer cs.Close(ctx)

ok := cs.Next(ctx)
next := cs.Current

```

</Tab>

<Tab name="Java (Sync)">

The Java examples below assume that you have [connected to a MongoDB replica set and have accessed a database](http://mongodb.github.io/mongo-java-driver/3.6/driver/tutorials/databases-collections/) that contains an `inventory` collection.

```java
MongoCursor<ChangeStreamDocument<Document>> cursor = inventory.watch().iterator();
ChangeStreamDocument<Document> next = cursor.next();
```

</Tab>

<Tab name="Kotlin (Coroutine)">

The Kotlin examples below assume that you are connected to a MongoDB replica set and can access a database that contains the `inventory` collection. To learn more about completing these tasks, see the [Kotlin Coroutine Driver Databases and Collections](https://www.mongodb.com/docs/drivers/kotlin/coroutine/current/fundamentals/databases-collections/) guide.

```kotlin
val job = launch {
  val changeStream = collection.watch()
  changeStream.collect {
    println("Received a change event: $it")
  }
}
```

</Tab>

<Tab name="Motor">

The examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://motor.readthedocs.io/en/stable/tutorial-asyncio.html#creating-a-client) that contains an `inventory` collection.

```python
cursor = db.inventory.watch()
document = await cursor.next()
```

</Tab>

<Tab name="Node.js">

The Node.js examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://mongodb.github.io/node-mongodb-native/api-generated/mongoclient.html#connect) that contains an `inventory` collection.

The following example uses stream to process the change events.

```javascript
const collection = db.collection('inventory');
const changeStream = collection.watch();
changeStream
        .on('change', next => {
          // process next document
        })
        .once('error', () => {
          // handle error
        });
```

Alternatively, you can also use iterator to process the change events:

```javascript
const collection = db.collection('inventory');
const changeStream = collection.watch();
const next = await changeStream.next();
```

ChangeStream extends [EventEmitter](https://mongodb.github.io/node-mongodb-native/5.7/classes/TypedEventEmitter.html).

</Tab>

<Tab name="PHP">

The examples below assume that you have [connected to a MongoDB replica  set and have accessed a database](https://www.mongodb.com/docs/php-library/current/reference/method/MongoDBClient__construct/) that contains an `inventory` collection.

```php
$changeStream = $db->inventory->watch();
$changeStream->rewind();

$firstChange = $changeStream->current();

$changeStream->next();

$secondChange = $changeStream->current();
```

</Tab>

<Tab name="Python">

The Python examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://www.mongodb.com/docs/drivers/pymongo/) that contains an `inventory` collection.

```python
cursor = db.inventory.watch()
next(cursor)
```

</Tab>

<Tab name="Ruby">

The examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://www.mongodb.com/docs/ruby-driver/current/reference/create-client/) that contains an `inventory` collection.

```ruby

cursor = inventory.watch.to_enum
cursor.next

```

</Tab>

<Tab name="Swift (Async)">

The Swift (Async) examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://mongodb.github.io/mongo-swift-driver/docs/current/MongoSwift/Classes/MongoClient.html) that contains an `inventory` collection.

```swift
let inventory = db.collection("inventory")

// Option 1: retrieve next document via next()
let next = inventory.watch().flatMap { cursor in
    cursor.next()
}

// Option 2: register a callback to execute for each document
let result = inventory.watch().flatMap { cursor in
    cursor.forEach { event in
        // process event
        print(event)
    }
}
```

</Tab>

<Tab name="Swift (Sync)">

The Swift (Sync) examples below assume that you have [connected to a MongoDB replica set and have accessed a database](https://mongodb.github.io/mongo-swift-driver/docs/current/MongoSwiftSync/Classes/MongoClient.html) that contains an `inventory` collection.

```swift
let inventory = db.collection("inventory")
let changeStream = try inventory.watch()
let next = changeStream.next()
```

</Tab>

</Tabs>

To retrieve the [data change event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) from the cursor, iterate the change stream cursor. For information on the change stream event, see [Change Events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output).

The [change stream cursor](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-changeStreams) remains open until one of the following occurs:

- The cursor is explicitly closed.

- An [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate) occurs; for example, a collection drop or rename.

- The connection to the MongoDB deployment closes or times out. See [Behavior](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/cursors/#std-label-cursor-behaviors) for more information.

- If the deployment is a sharded cluster, a shard removal may cause an open change stream cursor to close. The closed change stream cursor may not be fully resumable.

The lifecycle of an unclosed cursor is language-dependent.

You can specify a `startAtOperationTime` to open the cursor at a particular point in time. If the specified starting point is in the past, it must be in the time range of the oplog.

## Modify Change Stream Output

➤ Use the **Select your language** drop-down menu in the upper-right to set the language of the examples on this page.

<Tabs>

<Tab name="C">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```c
pipeline = BCON_NEW ("pipeline",
                     "[",
                     "{",
                     "$match",
                     "{",
                     "fullDocument.username",
                     BCON_UTF8 ("alice"),
                     "}",
                     "}",
                     "{",
                     "$addFields",
                     "{",
                     "newField",
                     BCON_UTF8 ("this is an added field!"),
                     "}",
                     "}",
                     "]");

stream = mongoc_collection_watch (collection, pipeline, &opts);
mongoc_change_stream_next (stream, &change);
if (mongoc_change_stream_error_document (stream, &error, NULL)) {
   MONGOC_ERROR ("%s\n", error.message);
}

mongoc_change_stream_destroy (stream);
```

</Tab>

<Tab name="C#">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```csharp
var pipeline = new EmptyPipelineDefinition<ChangeStreamDocument<BsonDocument>>()
    .Match(change =>
        change.FullDocument["username"] == "alice" ||
        change.OperationType == ChangeStreamOperationType.Delete)
    .AppendStage<ChangeStreamDocument<BsonDocument>, ChangeStreamDocument<BsonDocument>, BsonDocument>(
        "{ $addFields : { newField : 'this is an added field!' } }");

var collection = database.GetCollection<BsonDocument>("inventory");
using (var cursor = collection.Watch(pipeline))
{
    while (cursor.MoveNext() && cursor.Current.Count() == 0) { } // keep calling MoveNext until we've read the first batch
    var next = cursor.Current.First();
}
```

</Tab>

<Tab name="Go">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```go
pipeline := mongo.Pipeline{bson.D{{
	"$match", bson.D{{
		"$or",
		bson.A{
			bson.D{{"fullDocument.username", "alice"}},
			bson.D{{"operationType", "delete"}},
		},
	}},
}}}
cs, err := coll.Watch(ctx, pipeline)
assert.NoError(t, err)
defer cs.Close(ctx)

ok := cs.Next(ctx)
next := cs.Current

```

</Tab>

<Tab name="Java (Sync)">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```java
MongoClient mongoClient = MongoClients.create("mongodb://<username>:<password>@<host>:<port>");

// Select the MongoDB database and collection to open the change stream against

MongoDatabase db = mongoClient.getDatabase("myTargetDatabase");

MongoCollection<Document> collection = db.getCollection("myTargetCollection");

// Create $match pipeline stage.
List<Bson> pipeline = singletonList(Aggregates.match(Filters.or(
        Document.parse("{'fullDocument.username': 'alice'}"),
        Filters.in("operationType", asList("delete")))));

// Create the change stream cursor, passing the pipeline to the
// collection.watch() method

MongoCursor<Document> cursor = collection.watch(pipeline).iterator();
```

The `pipeline` list includes a single [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match) stage that filters for any operations that meet one or both of the following criteria:

- `username` value is `alice`

- `operationType` value is `delete`

Passing the `pipeline` to the [`watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method directs the change stream to return notifications after passing them through the specified `pipeline`.

</Tab>

<Tab name="Kotlin (Coroutine)">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```kotlin
val pipeline = listOf(
  Aggregates.match(
    or(
      eq("fullDocument.username", "alice"),
      `in`("operationType", listOf("delete"))
    )
  ))

val job = launch {
  val changeStream = collection.watch(pipeline)
  changeStream.collect {
    println("Received a change event: $it")
  }
}
```

The `pipeline` list includes a single [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match) stage that filters for any operations that meet one or both of the following criteria:

- `username` value is `alice`

- `operationType` value is `delete`

Passing the `pipeline` to the [`watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method directs the change stream to return notifications after passing them through the specified `pipeline`.

</Tab>

<Tab name="Motor">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```python
pipeline = [
    {"$match": {"fullDocument.username": "alice"}},
    {"$addFields": {"newField": "this is an added field!"}},
]
cursor = db.inventory.watch(pipeline=pipeline)
document = await cursor.next()
```

</Tab>

<Tab name="Node.js">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

The following example uses stream to process the change events.

```javascript
const pipeline = [
  { $match: { 'fullDocument.username': 'alice' } },
  { $addFields: { newField: 'this is an added field!' } }
];

const collection = db.collection('inventory');
const changeStream = collection.watch(pipeline);
changeStream
        .on('change', next => {
          // process next document
        })
        .once('error', error => {
          // handle error
        });
```

Alternatively, you can also use iterator to process the change events:

```javascript
const changeStreamIterator = collection.watch(pipeline);
const next = await changeStreamIterator.next();
```

</Tab>

<Tab name="PHP">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```php
$pipeline = [
    ['$match' => ['fullDocument.username' => 'alice']],
    ['$addFields' => ['newField' => 'this is an added field!']],
];
$changeStream = $db->inventory->watch($pipeline);
$changeStream->rewind();

$firstChange = $changeStream->current();

$changeStream->next();

$secondChange = $changeStream->current();
```

</Tab>

<Tab name="Python">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```python
pipeline = [
    {"$match": {"fullDocument.username": "alice"}},
    {"$addFields": {"newField": "this is an added field!"}},
]
cursor = db.inventory.watch(pipeline=pipeline)
next(cursor)
```

</Tab>

<Tab name="Ruby">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

</Tab>

<Tab name="Swift (Async)">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```swift
let pipeline: [BSONDocument] = [
    ["$match": ["fullDocument.username": "alice"]],
    ["$addFields": ["newField": "this is an added field!"]]
]
let inventory = db.collection("inventory")

// Option 1: use next() to iterate
let next = inventory.watch(pipeline, withEventType: BSONDocument.self).flatMap { changeStream in
    changeStream.next()
}

// Option 2: register a callback to execute for each document
let result = inventory.watch(pipeline, withEventType: BSONDocument.self).flatMap { changeStream in
    changeStream.forEach { event in
        // process event
        print(event)
    }
}
```

</Tab>

<Tab name="Swift (Sync)">

You can control [change stream output](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) by providing an array of one or more of the following pipeline stages when configuring the change stream:

- [`$addFields`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/addFields/#mongodb-pipeline-pipe.-addFields)

- [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match)

- [`$project`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/project/#mongodb-pipeline-pipe.-project)

- [`$replaceRoot`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceRoot/#mongodb-pipeline-pipe.-replaceRoot)

- [`$replaceWith`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/replaceWith/#mongodb-pipeline-pipe.-replaceWith)

- [`$redact`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/redact/#mongodb-pipeline-pipe.-redact)

- [`$set`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/set/#mongodb-pipeline-pipe.-set)

- [`$unset`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/unset/#mongodb-pipeline-pipe.-unset)

```swift
let pipeline: [BSONDocument] = [
    ["$match": ["fullDocument.username": "alice"]],
    ["$addFields": ["newField": "this is an added field!"]]
]
let inventory = db.collection("inventory")
let changeStream = try inventory.watch(pipeline, withEventType: BSONDocument.self)
let next = changeStream.next()
```

</Tab>

</Tabs>

The [_id](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-event-id) field of the change stream event document act as the [resume token](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume). Do not use the pipeline to modify or remove the change stream event's `_id` field.

Starting in MongoDB 4.2, change streams will throw an exception if the [change stream aggregation pipeline](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-modify-output) modifies an event's [_id](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-event-id) field.

See [Change Events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) for more information on the change stream response document format.

## Lookup Full Document for Update Operations

By default, change streams only return the delta of fields during the update operation. However, you can configure the change stream to return the most current majority-committed version of the updated document.

The `updateLookup` operation reads the document identified by its shard key and document identifier from the collection. The collection is identified by its name and uses the collection data as it exists at the time the change stream is processed. Consider these scenarios:

- If the collection is renamed, no document is returned.

- If the collection is renamed and a new collection is created with the old name, then the lookup operation is performed on the new collection. If a matching document is found, it is returned.

For situations involving rapid deletions or traffic spikes, configuring `fullDocument: "updateLookup"` with a [`$match`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/match/#mongodb-pipeline-pipe.-match) filter can cause 'Resume Token Not Found' errors. This occurs when a document deletion causes the `fullDocument` field to return a null value, because there is no matching document, which then prevents the change stream from finding the resume token.

Instead, use Pre- and Post-Images with `fullDocumentBeforeChange: "whenAvailable"` and `fullDocument: "whenAvailable"`. See the [Change Streams with Document Pre- and Post-Images](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#std-label-db.collection.watch-change-streams-pre-and-post-images-example) section.

➤ Use the **Select your language** drop-down menu in the upper-right to set the language of the examples on this page.

<Tabs>

<Tab name="C">

To return the most current majority-committed version of the updated document, pass the `"fullDocument"` option with the `"updateLookup"` value to the `mongoc_collection_watch` method.

In the example below, all update operations notifications include a `fullDocument` field that represents the *current* version of the document affected by the update operation.

```c
BSON_APPEND_UTF8 (&opts, "fullDocument", "updateLookup");
stream = mongoc_collection_watch (collection, pipeline, &opts);
mongoc_change_stream_next (stream, &change);
if (mongoc_change_stream_error_document (stream, &error, NULL)) {
   MONGOC_ERROR ("%s\n", error.message);
}

mongoc_change_stream_destroy (stream);
```

</Tab>

<Tab name="C#">

To return the most current majority-committed version of the updated document, pass `"FullDocument = ChangeStreamFullDocumentOption.UpdateLookup"` to the [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method.

In the example below, all update operations notifications include a `FullDocument` field that represents the *current* version of the document affected by the update operation.

```csharp
var options = new ChangeStreamOptions { FullDocument = ChangeStreamFullDocumentOption.UpdateLookup };
var cursor = inventory.Watch(options);
while (cursor.MoveNext() && cursor.Current.Count() == 0) { } // keep calling MoveNext until we've read the first batch
var next = cursor.Current.First();
cursor.Dispose();
```

</Tab>

<Tab name="Go">

To return the most current majority-committed version of the updated document, `SetFullDocument(options.UpdateLookup)` change stream option.

```go

cs, err := coll.Watch(ctx, mongo.Pipeline{}, options.ChangeStream().SetFullDocument(options.UpdateLookup))
assert.NoError(t, err)
defer cs.Close(ctx)

ok := cs.Next(ctx)
next := cs.Current

```

</Tab>

<Tab name="Java (Sync)">

To return the most current majority-committed version of the updated document, pass `FullDocument.UPDATE_LOOKUP`  to the `db.collection.watch.fullDocument()` method.

In the example below, all update operations notifications include a `FullDocument` field that represents the *current* version of the document affected by the update operation.

```java
cursor = inventory.watch().fullDocument(FullDocument.UPDATE_LOOKUP).iterator();
next = cursor.next();
```

</Tab>

<Tab name="Kotlin (Coroutine)">

To return the most current majority-committed version of the updated document, pass `FullDocument.UPDATE_LOOKUP`  to the [ChangeStreamFlow.fullDocument()](https://mongodb.github.io/mongo-java-driver/5.6/apidocs/driver-kotlin-coroutine/mongodb-driver-kotlin-coroutine/com.mongodb.kotlin.client.coroutine/-change-stream-flow/full-document.html) method.

In the example below, all update operations notifications include a `FullDocument` field that represents the *current* version of the document affected by the update operation.

```kotlin
val job = launch {
  val changeStream = collection.watch()
    .fullDocument(FullDocument.UPDATE_LOOKUP)
  changeStream.collect {
    println(it)
  }
}
```

</Tab>

<Tab name="Motor">

To return the most current majority-committed version of the updated document, pass `full_document='updateLookup'` to the [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method.

In the example below, all update operations notifications include a ``full_document` field that represents the *current* version of the document affected by the update operation.

```python
cursor = db.inventory.watch(full_document="updateLookup")
document = await cursor.next()
```

</Tab>

<Tab name="Node.js">

To return the most current majority-committed version of the updated document, pass `{ fullDocument: 'updateLookup' }` to the [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method.

In the example below, all update operations notifications include a `fullDocument` field that represents the *current* version of the document affected by the update operation.

The following example uses stream to process the change events.

```javascript
const collection = db.collection('inventory');
const changeStream = collection.watch([], { fullDocument: 'updateLookup' });
changeStream
        .on('change', next => {
          // process next document
        })
        .once('error', error => {
          // handle error
        });
```

Alternatively, you can also use iterator to process the change events:

```javascript
const changeStreamIterator = collection.watch([], { fullDocument: 'updateLookup' });
const next = await changeStreamIterator.next();
```

</Tab>

<Tab name="PHP">

To return the most current majority-committed version of the updated document, pass `"fullDocument' => \MongoDB\Operation\ChangeStreamCommand::FULL_DOCUMENT_UPDATE_LOOKUP"` to the [`db.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.watch/#mongodb-method-db.watch) method.

In the example below, all update operations notifications include a `fullDocument` field that represents the *current* version of the document affected by the update operation.

```php
$changeStream = $db->inventory->watch([], ['fullDocument' => \MongoDB\Operation\Watch::FULL_DOCUMENT_UPDATE_LOOKUP]);
$changeStream->rewind();

$firstChange = $changeStream->current();

$changeStream->next();

$secondChange = $changeStream->current();
```

</Tab>

<Tab name="Python">

To return the most current majority-committed version of the updated document, pass `full_document='updateLookup'` to the [`db.collection.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#mongodb-method-db.collection.watch) method.

In the example below, all update operations notifications include a `full_document` field that represents the *current* version of the document affected by the update operation.

```python
cursor = db.inventory.watch(full_document="updateLookup")
next(cursor)
```

</Tab>

<Tab name="Ruby">

To return the most current majority-committed version of the updated document, pass `full_document: 'updateLookup'` to the [`db.watch()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.watch/#mongodb-method-db.watch) method.

In the example below, all update operations notifications include a `full_document` field that represents the *current* version of the document affected by the update operation.

```ruby

cursor = inventory.watch([], full_document: 'updateLookup').to_enum
cursor.next

```

</Tab>

<Tab name="Swift (Async)">

To return the most current majority-committed version of the updated document, pass `options: ChangeStreamOptions(fullDocument: .updateLookup)` to the `watch()` method.

```swift
let inventory = db.collection("inventory")

// Option 1: use next() to iterate
let next = inventory.watch(options: ChangeStreamOptions(fullDocument: .updateLookup))
    .flatMap { changeStream in
        changeStream.next()
    }

// Option 2: register a callback to execute for each document
let result = inventory.watch(options: ChangeStreamOptions(fullDocument: .updateLookup))
    .flatMap { changeStream in
        changeStream.forEach { event in
            // process event
            print(event)
        }
    }
```

</Tab>

<Tab name="Swift (Sync)">

To return the most current majority-committed version of the updated document, pass `options: ChangeStreamOptions(fullDocument: .updateLookup)` to the `watch()` method.

```swift
let inventory = db.collection("inventory")
let changeStream = try inventory.watch(options: ChangeStreamOptions(fullDocument: .updateLookup))
let next = changeStream.next()
```

</Tab>

</Tabs>

If there are one or more majority-committed operations that modified the updated document *after* the update operation but *before* the lookup, the full document returned may differ significantly from the document at the time of the update operation.

However, the deltas included in the change stream document always correctly describe the watched collection changes that applied to that change stream event.

The `fullDocument` field for an update event may be missing if one of the following is true:

- If the document is deleted or if the collection is dropped in between the update and the lookup.

- If the update changes the values for at least one of the fields in that collection's shard key.

See [Change Events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) for more information on the change stream response document format.

## Resume a Change Stream

Change streams are resumable by specifying a resume token to either [resumeAfter](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume-after) or [startAfter](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-start-after) when opening the cursor.

When you resume a change stream with a resume token, use the same pipeline and options as when you originally generated the token. If you use a different change stream pipeline or different options, it might lead to unpredictable behavior, negatively impact data consistency, or prevent the change stream from resuming.

### `resumeAfter` for Change Streams

You can resume a change stream after a specific event by passing a resume token to `resumeAfter` when opening the cursor.

See [Resume Tokens](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume-token) for more information on the resume token.

- The oplog must have enough history to locate the operation associated with the token or the timestamp, if the timestamp is in the past.

- You cannot use `resumeAfter` to resume a change stream after an [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate) (for example, a collection drop or rename) closes the stream. Instead, you can use [startAfter](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-start-after) to start a new change stream after an [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate).

<Tabs>

<Tab name="C">

In the example below, the `resumeAfter` option is appended to the stream options to recreate the stream after it has been destroyed. Passing the `_id` to the change stream attempts to resume notifications starting after the operation specified.

```C
stream = mongoc_collection_watch (collection, pipeline, NULL);
if (mongoc_change_stream_next (stream, &change)) {
   resume_token = mongoc_change_stream_get_resume_token (stream);
   BSON_APPEND_DOCUMENT (&opts, "resumeAfter", resume_token);

   mongoc_change_stream_destroy (stream);
   stream = mongoc_collection_watch (collection, pipeline, &opts);
   mongoc_change_stream_next (stream, &change);
   mongoc_change_stream_destroy (stream);
} else {
   if (mongoc_change_stream_error_document (stream, &error, NULL)) {
      MONGOC_ERROR ("%s\n", error.message);
   }

   mongoc_change_stream_destroy (stream);
}
```

</Tab>

<Tab name="C#">

In the example below, the `resumeToken` is retrieved from the last change stream document and passed to the `Watch()` method as an option. Passing the `resumeToken` to the `Watch()` method directs the change stream to attempt to resume notifications starting after the operation specified in the resume token.

```csharp
  var resumeToken = previousCursor.GetResumeToken();
  var options = new ChangeStreamOptions { ResumeAfter = resumeToken };
  var cursor = inventory.Watch(options);
  cursor.MoveNext();
  var next = cursor.Current.First();
  cursor.Dispose();
```

</Tab>

<Tab name="Go">

You can use [ChangeStreamOptions.SetResumeAfter](https://godoc.org/go.mongodb.org/mongo-driver/mongo/options#ChangeStreamOptions.SetResumeAfter) to specify the resume token for the change stream. If the resumeAfter option is set, the change stream resumes notifications after the operation specified in the resume token. The `SetResumeAfter` takes a value that must resolve to a resume token, e.g. `resumeToken` in the example below.

```go
resumeToken := original.ResumeToken()

cs, err := coll.Watch(ctx, mongo.Pipeline{}, options.ChangeStream().SetResumeAfter(resumeToken))
assert.NoError(t, err)
defer cs.Close(ctx)

ok = cs.Next(ctx)
result := cs.Current

```

</Tab>

<Tab name="Java (Sync)">

You can use the `resumeAfter()` method to resume notifications after the operation specified in the resume token. The `resumeAfter()` method takes a value that must resolve to a resume token, e.g. `resumeToken` in the example below.

```java
BsonDocument resumeToken = next.getResumeToken();
cursor = inventory.watch().resumeAfter(resumeToken).iterator();
next = cursor.next();
```

</Tab>

<Tab name="Kotlin (Coroutine)">

You can use the [ChangeStreamFlow.resumeAfter()](https://mongodb.github.io/mongo-java-driver/5.6/apidocs/driver-kotlin-coroutine/mongodb-driver-kotlin-coroutine/com.mongodb.kotlin.client.coroutine/-change-stream-flow/resume-after.html) method to resume notifications after the operation specified in the resume token. The `resumeAfter()` method takes a value that must resolve to a resume token, such as the `resumeToken` variable in the example below.

```kotlin
val resumeToken = BsonDocument()
val job = launch {
  val changeStream = collection.watch()
    .resumeAfter(resumeToken)
  changeStream.collect {
    println(it)
  }
}
```

</Tab>

<Tab name="Motor">

You can use the `resume_after` modifier to resume notifications after the operation specified in the resume token. The `resume_after` modifier takes a value that must resolve to a resume token, e.g. `resume_token` in the example below.

```python
resume_token = cursor.resume_token
cursor = db.inventory.watch(resume_after=resume_token)
document = await cursor.next()
```

</Tab>

<Tab name="Node.js">

You can use the `resumeAfter` option to resume notifications after the operation specified in the resume token. The `resumeAfter` option takes a value that must resolve to a resume token, e.g. `resumeToken` in the example below.

```javascript
const collection = db.collection('inventory');
const changeStream = collection.watch();

let newChangeStream;
changeStream
        .once('change', next => {
          const resumeToken = changeStream.resumeToken;
          changeStream.close();

          newChangeStream = collection.watch([], { resumeAfter: resumeToken });
          newChangeStream
                  .on('change', next => {
                    processChange(next);
                  })
                  .once('error', error => {
                    // handle error
                  });
        })
        .once('error', error => {
          // handle error
        });
```

</Tab>

<Tab name="PHP">

You can use the `resumeAfter` option to resume notifications after the operation specified in the resume token. The `resumeAfter` option takes a value that must resolve to a resume token, e.g. `$resumeToken` in the example below.

```php
$resumeToken = $changeStream->getResumeToken();

if ($resumeToken === null) {
    throw new \Exception('Resume token was not found');
}

$changeStream = $db->inventory->watch([], ['resumeAfter' => $resumeToken]);
$changeStream->rewind();

$firstChange = $changeStream->current();
```

</Tab>

<Tab name="Python">

You can use the `resume_after` modifier to resume notifications after the operation specified in the resume token. The `resume_after` modifier takes a value that must resolve to a resume token, e.g. `resume_token` in the example below.

```python
resume_token = cursor.resume_token
cursor = db.inventory.watch(resume_after=resume_token)
next(cursor)
```

</Tab>

<Tab name="Ruby">

You can use the `resume_after` modifier to resume notifications after the operation specified in the resume token. The `resume_after` modifier takes a value that must resolve to a resume token, e.g. `resume_token` in the example below.

```ruby

  change_stream = inventory.watch
  cursor = change_stream.to_enum
  next_change = cursor.next
  resume_token = change_stream.resume_token

  new_cursor = inventory.watch([], resume_after: resume_token).to_enum
  new_cursor.next

```

</Tab>

<Tab name="Swift (Async)">

You can use the `resumeAfter` option to resume notifications after the operation specified in the resume token. The `resumeAfter` option takes a value that must resolve to a resume token, e.g. `resumeToken` in the example below.

```swift
let inventory = db.collection("inventory")

inventory.watch(options: ChangeStreamOptions(fullDocument: .updateLookup))
    .flatMap { changeStream in
        changeStream.next().map { _ in
            changeStream.resumeToken
        }.always { _ in
            _ = changeStream.kill()
        }
    }.flatMap { resumeToken in
        inventory.watch(options: ChangeStreamOptions(resumeAfter: resumeToken)).flatMap { newStream in
            newStream.forEach { event in
                // process event
                print(event)
            }
        }
    }
```

</Tab>

<Tab name="Swift (Sync)">

You can use the `resumeAfter` option to resume notifications after the operation specified in the resume token. The `resumeAfter` option takes a value that must resolve to a resume token, e.g. `resumeToken` in the example below.

```swift
let inventory = db.collection("inventory")
let changeStream = try inventory.watch(options: ChangeStreamOptions(fullDocument: .updateLookup))
let next = changeStream.next()

let resumeToken = changeStream.resumeToken
let resumedChangeStream = try inventory.watch(options: ChangeStreamOptions(resumeAfter: resumeToken))
let nextAfterResume = resumedChangeStream.next()
```

</Tab>

</Tabs>

### `startAfter` for Change Streams

You can start a new change stream after a specific event by passing a resume token to `startAfter` when opening the cursor. Unlike [resumeAfter](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume-after), `startAfter` can resume notifications after an [invalidate event](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/invalidate/#std-label-change-event-invalidate) by creating a new change stream.

See [Resume Tokens](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-resume-token) for more information on the resume token.

- The oplog must have enough history to locate the operation associated with the token or the timestamp, if the timestamp is in the past.

### Resume Tokens

There are two types of resume tokens:

- **Event token**: Identifies a specific change event. The change stream cursor generates an event token each time a change event occurs.

- **Highwatermark token**: Represents a point in time without an associated change event. The server periodically generates highwatermark tokens to indicate that cluster time has advanced, even when no change events occur.

The server periodically advances the timestamp in highwatermark resume tokens. On idle shards with infrequent writes, this advancement might not occur frequently enough for some use cases. To advance the highwatermark timestamp more frequently, you can write no-op entries to the oplog on idle shards using the [`appendOplogNote`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/appendOplogNote/#mongodb-dbcommand-dbcmd.appendOplogNote) command.

You can find resume tokens in multiple sources:

<table>
<tr>
<th id="Source">
Source

</th>
<th id="Description">
Description

</th>
</tr>
<tr>
<td headers="Source">
[Change Events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-event-resume-token)

</td>
<td headers="Description">
Each change event notification includes a resume token on the `_id` field.

</td>
</tr>
<tr>
<td headers="Source">
[Aggregation](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-aggregate-resume-token)

</td>
<td headers="Description">
The [`$changeStream`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/changeStream/#mongodb-pipeline-pipe.-changeStream) aggregation stage includes a resume token on the `cursor.postBatchResumeToken` field.

This field only appears when using the [`aggregate`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/aggregate/#mongodb-dbcommand-dbcmd.aggregate) command.

</td>
</tr>
<tr>
<td headers="Source">
[Get More](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-getMore-resume-token)

</td>
<td headers="Description">
The [`getMore`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/getMore/#mongodb-dbcommand-dbcmd.getMore) command includes a resume token on the `cursor.postBatchResumeToken` field.

</td>
</tr>
</table>Starting in MongoDB 4.2, change streams will throw an exception if the [change stream aggregation pipeline](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-change-stream-modify-output) modifies an event's [_id](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-event-id) field.

MongoDB provides a ["snippet"](https://www.mongodb.com/docs/mongodb-shell/snippets/#std-label-snip-overview), an extension to [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh), that decodes hex-encoded resume tokens.

You can install and run the [resumetoken](https://github.com/mongodb-labs/mongosh-snippets/tree/main/snippets/resumetoken) snippet from [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh):

```javascript
snippet install resumetoken
decodeResumeToken('<RESUME TOKEN>')
```

You can also run [resumetoken](https://github.com/mongodb-labs/mongosh-snippets/tree/main/snippets/resumetoken) from the command line (without using [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh)) if `npm` is installed on your system:

```javascript
npx mongodb-resumetoken-decoder <RESUME TOKEN>
```

See the following for more details on:

- [resumetoken](https://github.com/mongodb-labs/mongosh-snippets/tree/main/snippets/resumetoken)

- [using snippets](https://www.mongodb.com/docs/mongodb-shell/snippets/working-with-snippets/#std-label-snip-using-snippets) in [`mongosh`](https://www.mongodb.com/docs/mongodb-shell/#mongodb-binary-bin.mongosh).

#### Resume Tokens from Change Events

Change event notifications include a resume token on the `_id` field:

```json
{
  "_id": {
    "_data": "82635019A0000000012B042C0100296E5A1004AB1154ACACD849A48C61756D70D3B21F463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B65790046645F69640064635019A078BE67426D7CF4D2000004"
  },
  "operationType": "insert",
  "clusterTime": Timestamp({ "t": 1666193824, "i": 1 }),
"collectionUUID": new UUID("ab1154ac-acd8-49a4-8c61-756d70d3b21f"),
"wallTime": ISODate("2022-10-19T15:37:04.604Z"),
"fullDocument": {
"_id": ObjectId("635019a078be67426d7cf4d2"'),
"name": "Giovanni Verga"
},
"ns": {
"db": "test",
"coll": "names"
},
"documentKey": {
"_id": ObjectId("635019a078be67426d7cf4d2")
}
}
```

#### Resume Tokens from `aggregate`

When using the [`aggregate`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/aggregate/#mongodb-dbcommand-dbcmd.aggregate) command, the [`$changeStream`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/operator/aggregation/changeStream/#mongodb-pipeline-pipe.-changeStream) aggregation stage includes a resume token on the `cursor.postBatchResumeToken` field:

```json
{
  "cursor": {
    "firstBatch": [],
    "postBatchResumeToken": {
      "_data": "8263515EAC000000022B0429296E1404"
    },
    "id": Long("4309380460777152828"),
    "ns": "test.names"
  },
  "ok": 1,
  "$clusterTime": {
    "clusterTime": Timestamp({ "t": 1666277036, "i": 1 }),
"signature": {
"hash": Binary(Buffer.from("0000000000000000000000000000000000000000", "hex"), 0),
"keyId": Long("0")
}
},
"operationTime": Timestamp({ "t": 1666277036, "i": 1 })
}
```

#### Resume Tokens from `getMore`

The [`getMore`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/getMore/#mongodb-dbcommand-dbcmd.getMore) command also includes a resume token on the `cursor.postBatchResumeToken` field:

```json
{
  "cursor": {
    "nextBatch": [],
    "postBatchResumeToken": {
      "_data": "8263515979000000022B0429296E1404"
    },
    "id": Long("7049907285270685005"),
    "ns": "test.names"
  },
  "ok": 1,
  "$clusterTime": {
    "clusterTime": Timestamp( { "t": 1666275705, "i": 1 } ),
"signature": {
"hash": Binary(Buffer.from("0000000000000000000000000000000000000000", "hex"), 0),
"keyId": Long("0")
}
},
"operationTime": Timestamp({ "t": 1666275705, "i": 1 })
}
```

## Use Cases

Change streams can benefit architectures with reliant business systems, informing downstream systems once data changes are durable. For example, change streams can save time for developers when implementing Extract, Transform, and Load (ETL) services, cross-platform synchronization, collaboration functionality, and notification services.

## Access Control

For deployments enforcing [Authentication on Self-Managed Deployments](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/authentication/#std-label-authentication) and [authorization](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/authorization/#std-label-authorization):

- To open a change stream against specific collection, applications must have privileges that grant [`changeStream`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-changeStream) and [`find`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-find) actions on the corresponding collection.

  ```javascript
  { resource: { db: <dbname>, collection: <collection> }, actions: [ "find", "changeStream" ] }
  ```

- To open a change stream on a single database, applications must have privileges that grant [`changeStream`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-changeStream) and [`find`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-find) actions on all non-`system` collections in the database.

  ```javascript
  { resource: { db: <dbname>, collection: "" }, actions: [ "find", "changeStream" ] }
  ```

- To open a change stream on an entire deployment, applications must have privileges that grant [`changeStream`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-changeStream) and [`find`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/privilege-actions/#mongodb-authaction-find) actions on all non-`system` collections for all databases in the deployment.

  ```javascript
  { resource: { db: "", collection: "" }, actions: [ "find", "changeStream" ] }
  ```

## Event Notification

Change streams only notify on data changes that have persisted to a majority of data-bearing members in the replica set. This ensures that notifications are triggered only by majority-committed changes that are durable in failure scenarios.

For example, consider a 3-member [replica set](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-replica-set) with a change stream cursor opened against the [primary](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-primary). If a client issues an insert operation, the change stream only notifies the application of the data change once that insert has persisted to a majority of data-bearing members.

If an operation is associated with a [transaction](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/transactions/), the change event document includes the `txnNumber` and the `lsid`.

## Collation

Change streams use `simple` binary comparisons unless an explicit collation is provided.

## Change Streams and Orphan Documents

Starting in MongoDB 5.3, during [range migration](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/core/sharding-balancer-administration/#std-label-range-migration-procedure), [change stream](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/changeStreams/#std-label-changeStreams) events are not generated for updates to [orphaned documents](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/glossary/#std-term-orphaned-document).

## Change Streams with Document Pre- and Post-Images

Starting in MongoDB 6.0, you can use [change stream events](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/change-events/#std-label-change-stream-output) to output the version of a document before and after changes (the document pre- and post-images):

- The pre-image is the document before it was replaced, updated, or deleted. There is no pre-image for an inserted document.

- The post-image is the document after it was inserted, replaced, or updated. There is no post-image for a deleted document.

- Enable `changeStreamPreAndPostImages` for a collection using [`db.createCollection()`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.createCollection/#mongodb-method-db.createCollection), [`create`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/create/#mongodb-dbcommand-dbcmd.create), or [`collMod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/command/collMod/#mongodb-dbcommand-dbcmd.collMod). For example, when using the `collMod` command:

  ```javascript
  db.runCommand( {
     collMod: <collection>,
     changeStreamPreAndPostImages: { enabled: true }
  } )
  ```

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

For complete examples with the change stream output, see [Change Streams with Document Pre- and Post-Images](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/method/db.collection.watch/#std-label-db.collection.watch-change-streams-pre-and-post-images-example).

If the [`initialSyncMethod`](https://mongodbcom-cdn.staging.corp.mongodb.com/docs/reference/parameters/#mongodb-parameter-param.initialSyncMethod) parameter for the cluster is `fileCopyBased`, then there is no impact on change stream listeners.

If `initialSyncMethod` is `logical` and a change stream is opened on a newly synchronized node and reads events from a point in time earlier than the completion of the logical initial sync, the pre- and post-images may be missing.


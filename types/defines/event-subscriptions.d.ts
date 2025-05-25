type EventSubscriptionEvent<
	Service extends string,
	Type extends string,
	Source,
	Payload = {},
> = {
	type: `cf.${Service}.${Type}`;
	source: { service: Service } & Source;
	payload: Payload;
	metadata: {
		accountId: string;
		eventId: string;
		eventSubscriptionId: string;
		eventTimestamp: string; // RFC3339 timestamp
		eventSchemaVersion: 1;
	};
};

// Super Slurper
export type EventMessageSuperSlurper =
	| EventSubscriptionEvent<"superSlurper", "jobStarted", { jobId: string }>
	| EventSubscriptionEvent<"superSlurper", "jobPaused", { jobId: string }>
	| EventSubscriptionEvent<"superSlurper", "jobResumed", { jobId: string }>
	| EventSubscriptionEvent<
			"superSlurper",
			"jobFinished",
			{ jobId: string },
			{
				totalObjectsCount: number;
				migratedObjectsCount: number;
				failedObjectsCount: number;
			}
	  >
	| EventSubscriptionEvent<"superSlurper", "jobAborted", { jobId: string }>;

// Workflows
type SubscriptionEventSourceWorkflows = {
	workflowId: string;
	workflowName: string;
	versionId: string;
	instanceId: string;
};
export type EventMessageWorkflows =
	| EventSubscriptionEvent<
			"workflows",
			"instanceQueued",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instanceStarted",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instancePaused",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instanceResumed",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instanceErrored",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instanceTerminated",
			SubscriptionEventSourceWorkflows
	  >
	| EventSubscriptionEvent<
			"workflows",
			"instanceFinished",
			SubscriptionEventSourceWorkflows
	  >;

export type EventMessage = EventMessageSuperSlurper | EventMessageWorkflows;

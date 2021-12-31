import sys
import requests

# need to install via pip
import hjson

'''Looks like'''
input = '''{ pull_number: 26, 
    conflictPrs: 
        [ { number: 25, 
            files: [ '.github/workflows/testfile' ], 
            conflicts: [ '.github/workflows/testfile' ] }
        { number: 24, 
            files: [ '.github/workflows/testfile' ], 
            conflicts: [ '.github/workflows/testfile' ] } ] }'''
'''stuff'''
def main():
    input = sys.argv[1]
    j_input = hjson.loads(input)
    print(j_input)


    our_pr_num = j_input['pull_number']
    our_pr_label = get_label(our_pr_num)
    conflictPrs = j_input['conflictPrs']

    good = []
    bad = []

    for conflict in conflictPrs:
        this_pr_num = conflict['number']
        print(this_pr_num)

        r = requests.get(f'https://api.github.com/repos/PastaPastaPasta/dash/pulls/{conflict["number"]}')
        print(r.json()['head']['label'])
        r = requests.get(f'https://github.com/PastaPastaPasta/dash/branches/pre_mergeable/{our_pr_label}...{get_label(this_pr_num)}')
        if "These branches can be automatically merged." in r.text:
            good.append(this_pr_num)
        elif "Can’t automatically merge" in r.text:
            bad.append(this_pr_num)
        else: 
            raise Exception("not mergeable or unmergable!")

    print("Not conflicting PRs: ", good)

    print("Conflicting PRs: ", bad)
    if len(bad) > 0:
        sys.exit(1)



def get_label(pr_num):
    returnrequests.get(f'https://api.github.com/repos/PastaPastaPasta/dash/pulls/{pr_num}').json()['head']['label']


if __name__ == "__main__":
    main()